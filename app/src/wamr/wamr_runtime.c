/*
 * WAMR runtime wrapper + WASM executor thread.
 *
 * The executor is the only thread that touches the WAMR runtime. It
 * consumes LOAD_MODULE / UNLOAD_MODULE / CALL messages from wasp_exec_q
 * (serialized, so a module can never be unloaded mid-call) and answers
 * the coordinator directly on wasp_tx_q.
 *
 * The runtime allocates exclusively from a static pool
 * (CONFIG_WASP_WAMR_HEAP_POOL_SIZE) so WASM memory is bounded and cannot
 * starve the kernel. One module at a time; the raw .wasm buffer stays
 * allocated (WAMR references it in interpreter mode) until unload.
 *
 * v1 calling convention: exported functions may only take and return
 * i32 values (max WASP_CALL_MAX_ARGS / WASP_CALL_MAX_RESULTS).
 */
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "wasm_export.h"

#include "wamr_runtime.h"
#include "wasp.h"

LOG_MODULE_REGISTER(wasp_wamr, LOG_LEVEL_INF);

/*
 * Lives in main SRAM, not TCM: instantiating a module requires one
 * contiguous allocation of at least a full 64 KiB WASM page for its
 * linear memory (plus runtime overhead), which can never fit in a
 * 64 KiB TCM. The payload heap uses the TCM instead (see main.c).
 */
static uint8_t wamr_heap_pool[CONFIG_WASP_WAMR_HEAP_POOL_SIZE] __noinit;

/* State of the (single) loaded module. All owned by the executor thread. */
static struct {
	uint8_t *buf; /* raw .wasm, from wasp_payload_heap */
	wasm_module_t module;
	wasm_module_inst_t inst;
	wasm_exec_env_t exec_env;
} loaded;

int wasp_wamr_init(void)
{
	RuntimeInitArgs init_args = {
		.mem_alloc_type = Alloc_With_Pool,
		.mem_alloc_option.pool = {
			.heap_buf = wamr_heap_pool,
			.heap_size = sizeof(wamr_heap_pool),
		},
	};

	if (!wasm_runtime_full_init(&init_args)) {
		LOG_ERR("wasm_runtime_full_init failed");
		return -ENOMEM;
	}

	LOG_INF("WAMR initialized (heap pool: %u bytes)", (unsigned)sizeof(wamr_heap_pool));
	return 0;
}

static void unload_module(void)
{
	if (loaded.exec_env != NULL) {
		wasm_runtime_destroy_exec_env(loaded.exec_env);
	}
	if (loaded.inst != NULL) {
		wasm_runtime_deinstantiate(loaded.inst);
	}
	if (loaded.module != NULL) {
		wasm_runtime_unload(loaded.module);
	}
	if (loaded.buf != NULL) {
		k_heap_free(&wasp_payload_heap, loaded.buf);
	}
	memset(&loaded, 0, sizeof(loaded));
}

/*
 * Load + instantiate the module in msg->payload. On success the executor
 * takes ownership of the payload buffer; on failure it is freed here.
 */
static void handle_load(struct wasp_msg *msg)
{
	char err[100];

	if (loaded.module != NULL) {
		wasp_queue_error(msg, WASP_ERR_BUSY, "module already loaded, unload first");
		goto free_payload;
	}
	if (msg->len == 0) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "empty module");
		return;
	}

	loaded.buf = msg->payload;
	loaded.module = wasm_runtime_load(loaded.buf, msg->len, err, sizeof(err));
	if (loaded.module == NULL) {
		LOG_WRN("module load failed: %s", err);
		wasp_queue_error(msg, WASP_ERR_LOAD_FAILED, err);
		goto unload;
	}

	loaded.inst = wasm_runtime_instantiate(loaded.module, CONFIG_WASP_WASM_STACK_SIZE,
					       CONFIG_WASP_WASM_HEAP_SIZE, err, sizeof(err));
	if (loaded.inst == NULL) {
		LOG_WRN("module instantiation failed: %s", err);
		wasp_queue_error(msg, WASP_ERR_LOAD_FAILED, err);
		goto unload;
	}

	loaded.exec_env = wasm_runtime_create_exec_env(loaded.inst, CONFIG_WASP_WASM_STACK_SIZE);
	if (loaded.exec_env == NULL) {
		LOG_WRN("exec env creation failed");
		wasp_queue_error(msg, WASP_ERR_NO_MEM, "exec env creation failed");
		goto unload;
	}

	LOG_INF("module loaded and instantiated (%u bytes)", msg->len);
	wasp_queue_tx(msg, WASP_MSG_RESULT, NULL, 0);
	return;

unload:
	unload_module();
	return;
free_payload:
	if (msg->payload != NULL) {
		k_heap_free(&wasp_payload_heap, msg->payload);
	}
}

static void handle_unload(struct wasp_msg *msg)
{
	if (loaded.module == NULL) {
		wasp_queue_error(msg, WASP_ERR_NO_MODULE, NULL);
		return;
	}
	unload_module();
	LOG_INF("module unloaded");
	wasp_queue_tx(msg, WASP_MSG_RESULT, NULL, 0);
}

/*
 * CALL payload: [name_len u8][name][argc u8][arg u32 LE × argc]
 * Reply: RESULT [nresults u8][value u32 LE × nresults], or ERROR.
 */
static void handle_call(const struct wasp_msg *msg)
{
	char name[WASP_CALL_MAX_NAME_LEN + 1];
	wasm_valkind_t kinds[MAX(WASP_CALL_MAX_ARGS, WASP_CALL_MAX_RESULTS)];
	uint32_t cells[MAX(WASP_CALL_MAX_ARGS, WASP_CALL_MAX_RESULTS)];
	const uint8_t *p = msg->payload;
	uint32_t name_len, argc;

	if (loaded.inst == NULL) {
		wasp_queue_error(msg, WASP_ERR_NO_MODULE, NULL);
		return;
	}

	/* -- parse -- */
	if (msg->len < 2) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "short CALL payload");
		return;
	}
	name_len = p[0];
	if (name_len == 0 || name_len > WASP_CALL_MAX_NAME_LEN || msg->len < 2u + name_len) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "bad name length");
		return;
	}
	memcpy(name, &p[1], name_len);
	name[name_len] = '\0';

	argc = p[1 + name_len];
	if (argc > WASP_CALL_MAX_ARGS || msg->len != 2u + name_len + 4u * argc) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "bad argument count/length");
		return;
	}
	for (uint32_t i = 0; i < argc; i++) {
		cells[i] = sys_get_le32(&p[2 + name_len + 4 * i]);
	}

	/* -- signature check (v1: i32 only) -- */
	wasm_function_inst_t func = wasm_runtime_lookup_function(loaded.inst, name);

	if (func == NULL) {
		wasp_queue_error(msg, WASP_ERR_NO_FUNC, name);
		return;
	}

	uint32_t param_count = wasm_func_get_param_count(func, loaded.inst);
	uint32_t result_count = wasm_func_get_result_count(func, loaded.inst);

	if (param_count != argc) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "argument count mismatch");
		return;
	}
	if (result_count > WASP_CALL_MAX_RESULTS) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "too many results");
		return;
	}
	wasm_func_get_param_types(func, loaded.inst, kinds);
	for (uint32_t i = 0; i < param_count; i++) {
		if (kinds[i] != WASM_I32) {
			wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "non-i32 parameter");
			return;
		}
	}
	wasm_func_get_result_types(func, loaded.inst, kinds);
	for (uint32_t i = 0; i < result_count; i++) {
		if (kinds[i] != WASM_I32) {
			wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "non-i32 result");
			return;
		}
	}

	/* -- execute -- */
	if (!wasm_runtime_call_wasm(loaded.exec_env, func, argc, cells)) {
		const char *exception = wasm_runtime_get_exception(loaded.inst);

		LOG_WRN("call %s trapped: %s", name, exception ? exception : "?");
		wasp_queue_error(msg, WASP_ERR_TRAP, exception);
		wasm_runtime_clear_exception(loaded.inst);
		return;
	}

	/* -- reply: results are in cells[0..result_count) -- */
	uint8_t reply[1 + 4 * WASP_CALL_MAX_RESULTS];

	reply[0] = result_count;
	for (uint32_t i = 0; i < result_count; i++) {
		sys_put_le32(cells[i], &reply[1 + 4 * i]);
	}
	wasp_queue_tx(msg, WASP_MSG_RESULT, reply, 1 + 4 * result_count);
}

static void exec_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (wasp_wamr_init() != 0) {
		return;
	}

	while (true) {
		struct wasp_msg msg;

		if (k_msgq_get(&wasp_exec_q, &msg, K_FOREVER) != 0) {
			continue;
		}

		switch (msg.type) {
		case WASP_MSG_LOAD_MODULE:
			handle_load(&msg); /* takes/frees payload itself */
			continue;
		case WASP_MSG_UNLOAD_MODULE:
			handle_unload(&msg);
			break;
		case WASP_MSG_CALL:
			handle_call(&msg);
			break;
		default:
			LOG_ERR("unexpected type 0x%02x on exec queue", msg.type);
			wasp_queue_error(&msg, WASP_ERR_INTERNAL, NULL);
			break;
		}

		if (msg.payload != NULL) {
			k_heap_free(&wasp_payload_heap, msg.payload);
		}
	}
}

K_THREAD_DEFINE(wasp_exec_tid, CONFIG_WASP_EXEC_THREAD_STACK_SIZE, exec_thread_fn, NULL, NULL,
		NULL, K_PRIO_PREEMPT(7), 0, 0);
