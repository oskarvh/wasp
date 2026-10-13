/*
 * WAMR runtime wrapper + the WASM executor pool.
 *
 * Two tables, mirroring WAMR's own split. A *module* is validated
 * bytecode (wasm_module_t) plus the raw .wasm buffer, which has to stay
 * allocated because the interpreter references it. An *instance* is an
 * instantiation of a module (wasm_module_inst_t) with its own linear
 * memory and exec_env. Several instances may share one module, and
 * unrelated modules coexist — nothing ties a node's instances to one
 * program.
 *
 * CONFIG_WASP_MAX_INSTANCES executor threads drain the shared
 * wasp_exec_q. Any thread handles any message, which is what lets an
 * instance blocked on a remote-memory round trip sit out of the way while
 * other instances run. Two constraints keep that safe:
 *
 *   - A single mutex (table_lock) guards both tables. It is held across
 *     load, instantiate and drop, but never across a WASM call.
 *   - An instance carries a `running` flag, claimed under that mutex, so
 *     only one call executes per instance at a time. A wasm_module_inst_t
 *     and its exec_env are not re-entrant; a second concurrent CALL_INST
 *     on the same instance is answered ERROR(BUSY) rather than serialised,
 *     because a coordinator that did that wants to know.
 *
 * The runtime allocates exclusively from one pool (see wasp_wamr_init) so
 * WASM memory is bounded and cannot starve the kernel.
 *
 * The v1 single-module messages (LOAD_MODULE / UNLOAD_MODULE / CALL) are
 * kept as sugar for a default instance, so clients predating the instance
 * handles keep working unchanged.
 *
 * v1 calling convention: exported functions may only take and return
 * i32 values (max WASP_CALL_MAX_ARGS / WASP_CALL_MAX_RESULTS).
 */
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "wasm_export.h"

#include "inst_ctx.h"
#include "wamr_runtime.h"
#include "wasp.h"

LOG_MODULE_REGISTER(wasp_wamr, LOG_LEVEL_INF);

/*
 * The WAMR pool lives in main SRAM, not TCM: instantiating a module needs
 * one contiguous allocation of at least a full 64 KiB WASM page for its
 * linear memory (plus runtime overhead), which can never fit in a 64 KiB
 * TCM. The payload heap uses the TCM instead (see main.c).
 *
 * How big it is depends on the board, and deliberately is not spelled out
 * per board. With CONFIG_WASP_WAMR_POOL_FILL_RAM the pool is whatever RAM
 * is left over once every static allocation has been placed: the span from
 * _image_ram_end (the end of data, bss and noinit) to __kernel_ram_end
 * (the top of the zephyr,sram region). Nothing else is placed there — the
 * thread stacks, the interrupt stack and the kernel heap are all static
 * and therefore already below _image_ram_end — so the whole span is free.
 *
 * This is how the same firmware gets three concurrent module instances on
 * the H723's 320 KB and one on the F439's 192 KB without either number
 * appearing anywhere. It also beats a hand-tuned constant on both: the
 * static 88 KiB pool was sized for the F439 and left 33 KB of its SRAM
 * unused, while on the H723 it left 173 KB unused.
 *
 * Bounding WASM memory so it cannot starve the kernel still holds, because
 * everything the kernel needs is allocated before this runs. What changes
 * is that growing a static buffer now shrinks the pool instead of
 * overflowing RAM, so CONFIG_WASP_WAMR_HEAP_POOL_SIZE becomes a floor that
 * init refuses to boot below rather than the size itself.
 */
#if IS_ENABLED(CONFIG_WASP_WAMR_POOL_FILL_RAM)
#define POOL_ALIGN 8U
#else
static uint8_t wamr_heap_pool[CONFIG_WASP_WAMR_HEAP_POOL_SIZE] __noinit;
#endif

#define NSLOTS CONFIG_WASP_MAX_INSTANCES

struct mod_slot {
	uint8_t *buf; /* raw .wasm, from wasp_payload_heap */
	wasm_module_t module;
	uint8_t gen;
	uint8_t refs; /* instances derived from this module */
	bool used;
};

struct inst_slot {
	wasm_module_inst_t inst;
	wasm_exec_env_t exec_env;
	uint8_t mod_idx;
	uint8_t gen;
	bool used;
	bool running;
	/*
	 * Connection to answer on, plus this instance's counters. Reached
	 * from the host functions through
	 * wasm_runtime_get_user_data(exec_env) — conn used to be one global,
	 * which was correct only while a single thread could be mid-call.
	 */
	struct wasp_inst_ctx ctx;
};

static struct mod_slot mods[NSLOTS];
static struct inst_slot insts[NSLOTS];
static K_MUTEX_DEFINE(table_lock);

/* Resolve a handle to a slot, rejecting retired generations. NULL if the
 * index is out of range, the slot is free, or the generation has moved on.
 */
static struct inst_slot *inst_of(uint8_t handle)
{
	uint8_t idx = WASP_HANDLE_IDX(handle);

	if (idx >= NSLOTS || !insts[idx].used) {
		return NULL;
	}
	if (insts[idx].gen != WASP_HANDLE_GEN(handle)) {
		return NULL;
	}
	return &insts[idx];
}

static struct mod_slot *mod_of(uint8_t handle)
{
	uint8_t idx = WASP_HANDLE_IDX(handle);

	if (idx >= NSLOTS || !mods[idx].used) {
		return NULL;
	}
	if (mods[idx].gen != WASP_HANDLE_GEN(handle)) {
		return NULL;
	}
	return &mods[idx];
}

static uint8_t inst_handle(const struct inst_slot *s)
{
	return WASP_HANDLE(s - insts, s->gen);
}

static uint8_t mod_handle(const struct mod_slot *s)
{
	return WASP_HANDLE(s - mods, s->gen);
}

static size_t wamr_pool_size;

int wasp_wamr_init(void)
{
	uint8_t *pool;
	size_t pool_size;

#if IS_ENABLED(CONFIG_WASP_WAMR_POOL_FILL_RAM)
	uintptr_t start = ROUND_UP((uintptr_t)_image_ram_end, POOL_ALIGN);
	uintptr_t top = (uintptr_t)__kernel_ram_end;

	if (top <= start) {
		LOG_ERR("no RAM left for the WAMR pool");
		return -ENOMEM;
	}
	pool = (uint8_t *)start;
	pool_size = (size_t)(top - start);

	if (pool_size < CONFIG_WASP_WAMR_HEAP_POOL_SIZE) {
		LOG_ERR("only %zu bytes of RAM left, need %u — shrink a static "
			"buffer or lower CONFIG_WASP_WAMR_HEAP_POOL_SIZE",
			pool_size, (unsigned)CONFIG_WASP_WAMR_HEAP_POOL_SIZE);
		return -ENOMEM;
	}
#else
	pool = wamr_heap_pool;
	pool_size = sizeof(wamr_heap_pool);
#endif

	RuntimeInitArgs init_args = {
		.mem_alloc_type = Alloc_With_Pool,
		.mem_alloc_option.pool = {
			.heap_buf = pool,
			.heap_size = (uint32_t)pool_size,
		},
	};

	if (!wasm_runtime_full_init(&init_args)) {
		LOG_ERR("wasm_runtime_full_init failed");
		return -ENOMEM;
	}

	if (wasp_remote_register_natives() != 0) {
		wasm_runtime_destroy();
		return -EINVAL;
	}

	wamr_pool_size = pool_size;

	/* Report the instance budget, since that is what the number means in
	 * practice: one instance costs a 64 KiB linear-memory page plus the
	 * aux stack, the app heap and runtime overhead.
	 */
	LOG_INF("WAMR initialized (pool: %zu bytes at %p, %s, ~%zu instance(s))",
		pool_size, (void *)pool,
		IS_ENABLED(CONFIG_WASP_WAMR_POOL_FILL_RAM) ? "all spare RAM" : "static",
		pool_size / (size_t)(65536 + CONFIG_WASP_WASM_STACK_SIZE
				     + CONFIG_WASP_WASM_HEAP_SIZE));
	return 0;
}

/* Instance slots not currently in use. */
uint8_t wasp_wamr_free_slots(void)
{
	uint8_t free_slots = 0;

	k_mutex_lock(&table_lock, K_FOREVER);
	for (int i = 0; i < NSLOTS; i++) {
		if (!insts[i].used) {
			free_slots++;
		}
	}
	k_mutex_unlock(&table_lock);
	return free_slots;
}

/*
 * Bytes WAMR still has to hand out. Pool mode is the only mode
 * wasm_runtime_get_mem_alloc_info supports, and pool mode is what this
 * runs in, so this is the real figure rather than an estimate — which
 * matters because it is what tells a coordinator whether another
 * instance will fit before it asks for one.
 */
uint32_t wasp_wamr_pool_free(void)
{
	mem_alloc_info_t info;

	if (!wasm_runtime_get_mem_alloc_info(&info)) {
		return 0;
	}
	return info.total_free_size;
}

/* Caller holds table_lock. Tears down an instance and releases its
 * module reference, unloading the module once nothing refers to it.
 */
/* The v1 LOAD_MODULE/CALL/UNLOAD_MODULE trio addresses one implicit
 * instance. Declared here because drop_instance_locked has to invalidate
 * it: see the comment there.
 */
static uint8_t default_inst;
static bool default_valid;

/*
 * Counters of instances that no longer exist.
 *
 * The counters live on the instance, so dropping a module took its
 * measurements with it — and dropping the module is the last thing any
 * well-behaved client does, which made "run a workload, then ask what
 * happened" return nothing at all. Folding them in here keeps the totals
 * answerable after the fact; STATS reports them under handle 0xFF, which
 * can never name a real instance because the table is far smaller than
 * the sixteen slots a handle's index field could address.
 */
static struct wasp_inst_ctx retired;

static void fold_into_retired(const struct wasp_inst_ctx *c)
{
	retired.calls += c->calls;
	retired.traps += c->traps;
	retired.exec_ns += c->exec_ns;
	retired.rpc_count += c->rpc_count;
	retired.rpc_wait_ns += c->rpc_wait_ns;
	retired.rpc_tx += c->rpc_tx;
	retired.rpc_rx += c->rpc_rx;
}

static void drop_instance_locked(struct inst_slot *s)
{
	struct mod_slot *m = &mods[s->mod_idx];

	/*
	 * If this was the v1 default instance, forget it. Leaving the flag
	 * set while the slot is retired wedges the node: a v1 LOAD_MODULE
	 * answers BUSY because a default is supposedly loaded, while CALL
	 * and UNLOAD_MODULE answer NO_MODULE because the handle no longer
	 * resolves — and nothing short of a reboot clears it. Reachable from
	 * outside too, since DROP can remove an instance that v1 LOAD
	 * created.
	 *
	 * Here rather than in the DROP handler because this is the one place
	 * every instance teardown passes through: DROP of an instance, DROP
	 * of a module, and the rollback when a partial upload is undone.
	 */
	if (default_valid && default_inst == inst_handle(s)) {
		default_valid = false;
	}

	if (s->exec_env != NULL) {
		wasm_runtime_destroy_exec_env(s->exec_env);
	}
	if (s->inst != NULL) {
		wasm_runtime_deinstantiate(s->inst);
	}
	fold_into_retired(&s->ctx);
	s->inst = NULL;
	s->exec_env = NULL;
	s->running = false;
	s->used = false;
	s->gen = (s->gen + 1) & 0x0F; /* retire the handle */

	if (m->used && m->refs > 0) {
		m->refs--;
	}
}

/* Caller holds table_lock. Unloads bytecode; refuses while referenced. */
static void drop_module_locked(struct mod_slot *m)
{
	if (m->refs != 0) {
		return;
	}
	if (m->module != NULL) {
		wasm_runtime_unload(m->module);
	}
	if (m->buf != NULL) {
		k_heap_free(&wasp_payload_heap, m->buf);
	}
	m->module = NULL;
	m->buf = NULL;
	m->used = false;
	m->gen = (m->gen + 1) & 0x0F;
}

/* Caller holds table_lock. Drops every instance of a module, then it. */
static void drop_module_tree_locked(struct mod_slot *m)
{
	uint8_t idx = m - mods;

	for (int i = 0; i < NSLOTS; i++) {
		if (insts[i].used && insts[i].mod_idx == idx) {
			drop_instance_locked(&insts[i]);
		}
	}
	drop_module_locked(m);
}

/*
 * Caller holds table_lock. Instantiate one instance of mods[mod_idx].
 * Returns the handle, or -1 with *err describing why.
 */
static int instantiate_locked(uint8_t mod_idx, char *err, size_t errlen)
{
	struct mod_slot *m = &mods[mod_idx];
	struct inst_slot *s = NULL;

	for (int i = 0; i < NSLOTS; i++) {
		if (!insts[i].used) {
			s = &insts[i];
			break;
		}
	}
	if (s == NULL) {
		return -1;
	}

	s->inst = wasm_runtime_instantiate(m->module, CONFIG_WASP_WASM_STACK_SIZE,
					   CONFIG_WASP_WASM_HEAP_SIZE, err, errlen);
	if (s->inst == NULL) {
		return -1;
	}
	s->exec_env = wasm_runtime_create_exec_env(s->inst, CONFIG_WASP_WASM_STACK_SIZE);
	if (s->exec_env == NULL) {
		wasm_runtime_deinstantiate(s->inst);
		s->inst = NULL;
		strncpy(err, "exec env creation failed", errlen - 1);
		return -1;
	}

	/*
	 * The host functions reach the connection to answer on, and their
	 * own counters, through the exec_env rather than a global. Point at
	 * the context, not the slot, so native_remote.c needs no knowledge
	 * of this struct's layout.
	 */
	wasm_runtime_set_user_data(s->exec_env, &s->ctx);

	memset(&s->ctx, 0, sizeof(s->ctx));
	s->mod_idx = mod_idx;
	s->used = true;
	s->running = false;
	m->refs++;
	return inst_handle(s);
}

/*
 * Load bytecode into a module slot. Takes ownership of buf on success.
 * Returns the module index, or -1.
 */
static int load_locked(uint8_t *buf, uint32_t len, char *err, size_t errlen)
{
	struct mod_slot *m = NULL;

	for (int i = 0; i < NSLOTS; i++) {
		if (!mods[i].used) {
			m = &mods[i];
			break;
		}
	}
	if (m == NULL) {
		return -1;
	}

	m->module = wasm_runtime_load(buf, len, err, errlen);
	if (m->module == NULL) {
		return -1;
	}
	m->buf = buf;
	m->refs = 0;
	m->used = true;
	return m - mods;
}

/*
 * MODULE_UPLOAD: [n_inst u8][.wasm] -> RESULT [mod][n][inst × n].
 *
 * One frame both uploads and instantiates, so the ordinary single-instance
 * case stays one round trip while n_inst > 1 creates several instances
 * from one transfer of the bytecode. n_inst = 0 uploads only.
 */
static void handle_upload(struct wasp_msg *msg)
{
	char err[100] = "";
	uint8_t reply[2 + NSLOTS];
	uint32_t n_inst;
	int mod_idx;
	uint8_t created = 0;

	if (msg->len < 2) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "short MODULE_UPLOAD");
		goto free_payload;
	}
	n_inst = msg->payload[0];
	if (n_inst > NSLOTS) {
		wasp_queue_error(msg, WASP_ERR_NO_SLOTS, "more instances than slots");
		goto free_payload;
	}

	/*
	 * The .wasm has to outlive this message, and it sits one byte into
	 * the payload. Rather than keep the whole frame and remember the
	 * offset forever, copy the module out into its own allocation.
	 */
	uint32_t wasm_len = msg->len - 1;
	uint8_t *buf = k_heap_alloc(&wasp_payload_heap, wasm_len, K_MSEC(100));

	if (buf == NULL) {
		wasp_queue_error(msg, WASP_ERR_NO_MEM, "no room for the module");
		goto free_payload;
	}
	memcpy(buf, &msg->payload[1], wasm_len);

	k_mutex_lock(&table_lock, K_FOREVER);
	mod_idx = load_locked(buf, wasm_len, err, sizeof(err));
	if (mod_idx < 0) {
		k_mutex_unlock(&table_lock);
		k_heap_free(&wasp_payload_heap, buf);
		if (err[0] != '\0') {
			LOG_WRN("module load failed: %s", err);
			wasp_queue_error(msg, WASP_ERR_LOAD_FAILED, err);
		} else {
			wasp_queue_error(msg, WASP_ERR_NO_SLOTS, "module table full");
		}
		goto free_payload;
	}

	for (uint32_t i = 0; i < n_inst; i++) {
		int h = instantiate_locked(mod_idx, err, sizeof(err));

		if (h < 0) {
			break;
		}
		reply[2 + created++] = (uint8_t)h;
	}

	if (created < n_inst) {
		/* Partial success is worse than none: the coordinator asked
		 * for a specific width and would have to unpick the rest.
		 */
		drop_module_tree_locked(&mods[mod_idx]);
		k_mutex_unlock(&table_lock);
		LOG_WRN("could only instantiate %u of %u: %s", created, n_inst, err);
		wasp_queue_error(msg, WASP_ERR_NO_SLOTS, err[0] ? err : "pool exhausted");
		goto free_payload;
	}

	reply[0] = mod_handle(&mods[mod_idx]);
	reply[1] = created;
	k_mutex_unlock(&table_lock);

	LOG_INF("module %u uploaded (%u bytes), %u instance(s)", reply[0], wasm_len, created);
	wasp_queue_tx(msg, WASP_MSG_RESULT, reply, 2 + created);

free_payload:
	if (msg->payload != NULL) {
		k_heap_free(&wasp_payload_heap, msg->payload);
		msg->payload = NULL;
	}
}

/* INSTANTIATE: [mod][count] -> RESULT [count][inst × count]. */
static void handle_instantiate(struct wasp_msg *msg)
{
	char err[100] = "";
	uint8_t reply[1 + NSLOTS];
	struct mod_slot *m;
	uint32_t count;
	uint8_t created = 0;

	if (msg->len < 2) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "short INSTANTIATE");
		return;
	}
	count = msg->payload[1];
	if (count == 0 || count > NSLOTS) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "bad instance count");
		return;
	}

	k_mutex_lock(&table_lock, K_FOREVER);
	m = mod_of(msg->payload[0]);
	if (m == NULL) {
		k_mutex_unlock(&table_lock);
		wasp_queue_error(msg, WASP_ERR_BAD_HANDLE, "no such module");
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		int h = instantiate_locked(m - mods, err, sizeof(err));

		if (h < 0) {
			break;
		}
		reply[1 + created++] = (uint8_t)h;
	}
	if (created < count) {
		for (uint8_t i = 0; i < created; i++) {
			drop_instance_locked(inst_of(reply[1 + i]));
		}
		k_mutex_unlock(&table_lock);
		wasp_queue_error(msg, WASP_ERR_NO_SLOTS, err[0] ? err : "pool exhausted");
		return;
	}
	k_mutex_unlock(&table_lock);

	reply[0] = created;
	LOG_INF("module %u: %u more instance(s)", msg->payload[0], created);
	wasp_queue_tx(msg, WASP_MSG_RESULT, reply, 1 + created);
}

/* DROP: [kind][handle]. */
static void handle_drop(struct wasp_msg *msg)
{
	if (msg->len < 2) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "short DROP");
		return;
	}

	k_mutex_lock(&table_lock, K_FOREVER);
	if (msg->payload[0] == WASP_DROP_INSTANCE) {
		struct inst_slot *s = inst_of(msg->payload[1]);

		if (s == NULL) {
			k_mutex_unlock(&table_lock);
			wasp_queue_error(msg, WASP_ERR_BAD_HANDLE, "no such instance");
			return;
		}
		if (s->running) {
			k_mutex_unlock(&table_lock);
			wasp_queue_error(msg, WASP_ERR_BUSY, "instance is executing");
			return;
		}
		uint8_t mod_idx = s->mod_idx;

		drop_instance_locked(s);
		/* An instance was the only thing keeping its bytecode alive. */
		if (mods[mod_idx].used && mods[mod_idx].refs == 0) {
			drop_module_locked(&mods[mod_idx]);
		}
	} else if (msg->payload[0] == WASP_DROP_MODULE) {
		struct mod_slot *m = mod_of(msg->payload[1]);

		if (m == NULL) {
			k_mutex_unlock(&table_lock);
			wasp_queue_error(msg, WASP_ERR_BAD_HANDLE, "no such module");
			return;
		}
		for (int i = 0; i < NSLOTS; i++) {
			if (insts[i].used && insts[i].mod_idx == (m - mods) &&
			    insts[i].running) {
				k_mutex_unlock(&table_lock);
				wasp_queue_error(msg, WASP_ERR_BUSY,
						 "an instance is executing");
				return;
			}
		}
		drop_module_tree_locked(m);
	} else {
		k_mutex_unlock(&table_lock);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "bad DROP kind");
		return;
	}
	k_mutex_unlock(&table_lock);

	wasp_queue_tx(msg, WASP_MSG_RESULT, NULL, 0);
}

/*
 * STATS: per-instance time and traffic. [reset u8] clears afterwards, so a
 * caller can bracket a run rather than subtract two absolute readings.
 */
static void handle_stats(const struct wasp_msg *msg)
{
	uint8_t reply[1 + (NSLOTS + 1) * 29];
	bool reset = (msg->len >= 1 && msg->payload[0] != 0);
	uint32_t n = 1;
	uint8_t count = 0;

	k_mutex_lock(&table_lock, K_FOREVER);
	for (int i = 0; i < NSLOTS; i++) {
		struct inst_slot *s = &insts[i];

		if (!s->used) {
			continue;
		}
		reply[n++] = inst_handle(s);
		sys_put_le32(s->ctx.calls, &reply[n]);           n += 4;
		sys_put_le32(s->ctx.traps, &reply[n]);           n += 4;
		/* ns -> us on the way out; see protocol.h on the range. */
		sys_put_le32((uint32_t)(s->ctx.exec_ns / 1000U), &reply[n]);
		n += 4;
		sys_put_le32(s->ctx.rpc_count, &reply[n]);       n += 4;
		sys_put_le32((uint32_t)(s->ctx.rpc_wait_ns / 1000U), &reply[n]);
		n += 4;
		sys_put_le32(s->ctx.rpc_tx, &reply[n]);          n += 4;
		sys_put_le32(s->ctx.rpc_rx, &reply[n]);          n += 4;
		count++;

		if (reset) {
			uint8_t conn = s->ctx.conn;

			memset(&s->ctx, 0, sizeof(s->ctx));
			s->ctx.conn = conn; /* not a statistic */
		}
	}

	/* Always last, always present: a caller can then report totals
	 * without having to care whether the instances still exist.
	 */
	reply[n++] = 0xFF;
	sys_put_le32(retired.calls, &reply[n]);     n += 4;
	sys_put_le32(retired.traps, &reply[n]);     n += 4;
	sys_put_le32((uint32_t)(retired.exec_ns / 1000U), &reply[n]);     n += 4;
	sys_put_le32(retired.rpc_count, &reply[n]); n += 4;
	sys_put_le32((uint32_t)(retired.rpc_wait_ns / 1000U), &reply[n]); n += 4;
	sys_put_le32(retired.rpc_tx, &reply[n]);    n += 4;
	sys_put_le32(retired.rpc_rx, &reply[n]);    n += 4;
	count++;
	if (reset) {
		memset(&retired, 0, sizeof(retired));
	}

	k_mutex_unlock(&table_lock);
	reply[0] = count;

	wasp_queue_tx(msg, WASP_MSG_RESULT, reply, n);
}

/* INVENTORY: what is resident and what room is left. */
static void handle_inventory(const struct wasp_msg *msg)
{
	uint8_t reply[7 + NSLOTS * (2 + NSLOTS)];
	uint32_t n = 0;
	uint8_t free_slots = 0, n_mod = 0;

	k_mutex_lock(&table_lock, K_FOREVER);
	for (int i = 0; i < NSLOTS; i++) {
		if (!insts[i].used) {
			free_slots++;
		}
	}
	reply[0] = NSLOTS;
	reply[1] = free_slots;
	sys_put_le32(wasp_wamr_pool_free(), &reply[2]);
	n = 7;
	for (int i = 0; i < NSLOTS; i++) {
		if (!mods[i].used) {
			continue;
		}
		uint32_t at = n;

		reply[n++] = mod_handle(&mods[i]);
		reply[n++] = 0;
		for (int j = 0; j < NSLOTS; j++) {
			if (insts[j].used && insts[j].mod_idx == i) {
				reply[n++] = inst_handle(&insts[j]);
				reply[at + 1]++;
			}
		}
		n_mod++;
	}
	k_mutex_unlock(&table_lock);
	reply[6] = n_mod;

	wasp_queue_tx(msg, WASP_MSG_RESULT, reply, n);
}

static void release(struct inst_slot *slot)
{
	k_mutex_lock(&table_lock, K_FOREVER);
	slot->running = false;
	k_mutex_unlock(&table_lock);
}

/*
 * CALL payload: [name_len u8][name][argc u8][arg u32 LE × argc]
 * Reply: RESULT [nresults u8][value u32 LE × nresults], or ERROR.
 */
static void handle_call(const struct wasp_msg *msg, bool with_handle)
{
	char name[WASP_CALL_MAX_NAME_LEN + 1];
	wasm_valkind_t kinds[MAX(WASP_CALL_MAX_ARGS, WASP_CALL_MAX_RESULTS)];
	uint32_t cells[MAX(WASP_CALL_MAX_ARGS, WASP_CALL_MAX_RESULTS)];
	const uint8_t *p = msg->payload;
	uint32_t len = msg->len;
	uint32_t name_len, argc;
	struct inst_slot *slot;
	uint8_t handle;

	/*
	 * CALL_INST names its instance; the v1 CALL means the default one.
	 * Claiming `running` under the table lock is what keeps two executor
	 * threads out of the same wasm_module_inst_t — it is not re-entrant,
	 * and nothing else here would stop them.
	 */
	if (with_handle) {
		if (len < 1) {
			wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "short CALL_INST");
			return;
		}
		handle = p[0];
		p++;
		len--;
	} else {
		if (!default_valid) {
			wasp_queue_error(msg, WASP_ERR_NO_MODULE, NULL);
			return;
		}
		handle = default_inst;
	}

	k_mutex_lock(&table_lock, K_FOREVER);
	slot = inst_of(handle);
	if (slot == NULL) {
		k_mutex_unlock(&table_lock);
		wasp_queue_error(msg, with_handle ? WASP_ERR_BAD_HANDLE : WASP_ERR_NO_MODULE,
				 with_handle ? "no such instance" : NULL);
		return;
	}
	if (slot->running) {
		k_mutex_unlock(&table_lock);
		wasp_queue_error(msg, WASP_ERR_BUSY, "instance is already executing");
		return;
	}
	slot->running = true;
	slot->ctx.conn = msg->conn;
	k_mutex_unlock(&table_lock);

	/* -- parse -- */
	if (len < 2) {
		release(slot);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "short CALL payload");
		return;
	}
	name_len = p[0];
	if (name_len == 0 || name_len > WASP_CALL_MAX_NAME_LEN || len < 2u + name_len) {
		release(slot);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "bad name length");
		return;
	}
	memcpy(name, &p[1], name_len);
	name[name_len] = '\0';

	argc = p[1 + name_len];
	if (argc > WASP_CALL_MAX_ARGS || len != 2u + name_len + 4u * argc) {
		release(slot);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "bad argument count/length");
		return;
	}
	for (uint32_t i = 0; i < argc; i++) {
		cells[i] = sys_get_le32(&p[2 + name_len + 4 * i]);
	}

	/* -- signature check (v1: i32 only) -- */
	wasm_function_inst_t func = wasm_runtime_lookup_function(slot->inst, name);

	if (func == NULL) {
		release(slot);
		wasp_queue_error(msg, WASP_ERR_NO_FUNC, name);
		return;
	}

	uint32_t param_count = wasm_func_get_param_count(func, slot->inst);
	uint32_t result_count = wasm_func_get_result_count(func, slot->inst);

	if (param_count != argc) {
		release(slot);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "argument count mismatch");
		return;
	}
	if (result_count > WASP_CALL_MAX_RESULTS) {
		release(slot);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "too many results");
		return;
	}
	wasm_func_get_param_types(func, slot->inst, kinds);
	for (uint32_t i = 0; i < param_count; i++) {
		if (kinds[i] != WASM_I32) {
			release(slot);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "non-i32 parameter");
			return;
		}
	}
	wasm_func_get_result_types(func, slot->inst, kinds);
	for (uint32_t i = 0; i < result_count; i++) {
		if (kinds[i] != WASM_I32) {
			release(slot);
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "non-i32 result");
			return;
		}
	}

	/* -- execute -- */
	uint32_t t0 = k_cycle_get_32();
	bool ok = wasm_runtime_call_wasm(slot->exec_env, func, argc, cells);

	/*
	 * Wall time inside the interpreter, RPC waits included —
	 * subtracting ctx.rpc_wait_ns from this is what separates compute
	 * from transmission.
	 *
	 * Caveat: k_cycle_get_32 wraps every ~7.8 s at 550 MHz, so a single
	 * call lasting longer than that is undercounted. Calls here are
	 * milliseconds; a module that deliberately ran for minutes inside
	 * one CALL would need a 64-bit cycle counter.
	 */
	slot->ctx.exec_ns += k_cyc_to_ns_floor64(k_cycle_get_32() - t0);
	slot->ctx.calls++;

	if (!ok) {
		const char *exception = wasm_runtime_get_exception(slot->inst);

		LOG_WRN("call %s trapped: %s", name, exception ? exception : "?");
		release(slot);
		wasp_queue_error(msg, WASP_ERR_TRAP, exception);
		wasm_runtime_clear_exception(slot->inst);
		slot->ctx.traps++;
		release(slot);
		return;
	}

	/* -- reply: results are in cells[0..result_count) -- */
	uint8_t reply[1 + 4 * WASP_CALL_MAX_RESULTS];

	reply[0] = result_count;
	for (uint32_t i = 0; i < result_count; i++) {
		sys_put_le32(cells[i], &reply[1 + 4 * i]);
	}
	release(slot);
	wasp_queue_tx(msg, WASP_MSG_RESULT, reply, 1 + 4 * result_count);
}

/*
 * v1 LOAD_MODULE: upload and instantiate exactly one, and remember it as
 * the default so the v1 CALL and UNLOAD_MODULE keep working. Refuses a
 * second load the way it always did, since a v1 client has no way to name
 * which of two modules it meant.
 */
static void handle_load_v1(struct wasp_msg *msg)
{
	char err[100] = "";
	uint8_t *buf;
	int mod_idx, h;

	if (msg->len == 0) {
		wasp_queue_error(msg, WASP_ERR_BAD_ARGS, "empty module");
		goto done;
	}
	if (default_valid) {
		wasp_queue_error(msg, WASP_ERR_BUSY, "module already loaded, unload first");
		goto done;
	}

	buf = msg->payload;
	msg->payload = NULL; /* the module table takes it on success */

	k_mutex_lock(&table_lock, K_FOREVER);
	mod_idx = load_locked(buf, msg->len, err, sizeof(err));
	if (mod_idx < 0) {
		k_mutex_unlock(&table_lock);
		k_heap_free(&wasp_payload_heap, buf);
		LOG_WRN("module load failed: %s", err);
		wasp_queue_error(msg, err[0] ? WASP_ERR_LOAD_FAILED : WASP_ERR_NO_SLOTS, err);
		goto done;
	}
	h = instantiate_locked(mod_idx, err, sizeof(err));
	if (h < 0) {
		drop_module_tree_locked(&mods[mod_idx]);
		k_mutex_unlock(&table_lock);
		LOG_WRN("module instantiation failed: %s", err);
		wasp_queue_error(msg, WASP_ERR_LOAD_FAILED, err);
		goto done;
	}
	default_inst = (uint8_t)h;
	default_valid = true;
	k_mutex_unlock(&table_lock);

	LOG_INF("module loaded and instantiated (%u bytes)", msg->len);
	wasp_queue_tx(msg, WASP_MSG_RESULT, NULL, 0);

done:
	if (msg->payload != NULL) {
		k_heap_free(&wasp_payload_heap, msg->payload);
		msg->payload = NULL;
	}
}

/* v1 UNLOAD_MODULE: drop the default instance and its bytecode. */
static void handle_unload_v1(struct wasp_msg *msg)
{
	struct inst_slot *slot;

	k_mutex_lock(&table_lock, K_FOREVER);
	slot = default_valid ? inst_of(default_inst) : NULL;
	if (slot == NULL) {
		k_mutex_unlock(&table_lock);
		wasp_queue_error(msg, WASP_ERR_NO_MODULE, NULL);
		return;
	}
	if (slot->running) {
		k_mutex_unlock(&table_lock);
		wasp_queue_error(msg, WASP_ERR_BUSY, "instance is executing");
		return;
	}
	drop_module_tree_locked(&mods[slot->mod_idx]);
	default_valid = false;
	k_mutex_unlock(&table_lock);

	LOG_INF("module unloaded");
	wasp_queue_tx(msg, WASP_MSG_RESULT, NULL, 0);
}

static void exec_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		struct wasp_msg msg;

		if (k_msgq_get(&wasp_exec_q, &msg, K_FOREVER) != 0) {
			continue;
		}

		switch (msg.type) {
		/* These two free or hand on the payload themselves. */
		case WASP_MSG_LOAD_MODULE:
			handle_load_v1(&msg);
			continue;
		case WASP_MSG_MODULE_UPLOAD:
			handle_upload(&msg);
			continue;

		case WASP_MSG_UNLOAD_MODULE:
			handle_unload_v1(&msg);
			break;
		case WASP_MSG_INSTANTIATE:
			handle_instantiate(&msg);
			break;
		case WASP_MSG_DROP:
			handle_drop(&msg);
			break;
		case WASP_MSG_INVENTORY:
			handle_inventory(&msg);
			break;
		case WASP_MSG_STATS:
			handle_stats(&msg);
			break;
		case WASP_MSG_CALL:
			handle_call(&msg, false);
			break;
		case WASP_MSG_CALL_INST:
			handle_call(&msg, true);
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

/*
 * CONFIG_WASP_MAX_INSTANCES identical workers on one queue, rather than a
 * thread per instance plus a queue each. Any worker takes any message, so
 * a worker parked in wasm_runtime_call_wasm waiting on a remote-memory
 * round trip leaves the rest free to run other instances — the point of
 * the exercise. What stops two workers entering the same instance is the
 * `running` claim in handle_call, not thread affinity.
 *
 * One worker is started by wasm_runtime_init below; the rest wait on it,
 * because wasm_runtime_full_init must happen exactly once and before any
 * module is touched.
 */
static K_THREAD_STACK_ARRAY_DEFINE(exec_stacks, NSLOTS, CONFIG_WASP_EXEC_THREAD_STACK_SIZE);
static struct k_thread exec_threads[NSLOTS];

static int exec_pool_start(void)
{
	if (wasp_wamr_init() != 0) {
		return -ENODEV;
	}

	for (int i = 0; i < NSLOTS; i++) {
		k_thread_create(&exec_threads[i], exec_stacks[i],
				CONFIG_WASP_EXEC_THREAD_STACK_SIZE, exec_thread_fn,
				NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
		k_thread_name_set(&exec_threads[i], "wasp_exec");
	}
	LOG_INF("%d executor thread(s)", NSLOTS);
	return 0;
}

/* After the payload heap (priority 0) and the PSRAM probe, before the
 * network comes up and anything can arrive.
 */
SYS_INIT(exec_pool_start, POST_KERNEL, 3);

