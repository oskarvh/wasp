/*
 * WAMR runtime wrapper + WASM executor thread.
 *
 * The runtime allocates exclusively from a static pool
 * (CONFIG_WASP_WAMR_HEAP_POOL_SIZE) so WASM memory is bounded and cannot
 * starve the kernel. The executor thread is the only thread that calls
 * into a running module; it is separate from the agent so a misbehaving
 * module cannot wedge command handling.
 */
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>

#include "wasm_export.h"

#include "wamr_runtime.h"
#include "wasp.h"

LOG_MODULE_REGISTER(wasp_wamr, LOG_LEVEL_INF);

/* Prefer tightly-coupled memory for the pool when the board has it (e.g.
 * CCM on STM32F4 — CPU-only, which is fine: WAMR memory is never DMA'd).
 * This keeps main SRAM free for network buffers and thread stacks. */
#if DT_HAS_CHOSEN(zephyr_dtcm)
static uint8_t wamr_heap_pool[CONFIG_WASP_WAMR_HEAP_POOL_SIZE] __dtcm_noinit_section;
#else
static uint8_t wamr_heap_pool[CONFIG_WASP_WAMR_HEAP_POOL_SIZE] __noinit;
#endif

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

		/* TODO(exec): look up the exported function, run it via
		 * wasm_runtime_call_wasm(), post outcome to wasp_res_q. */
		LOG_WRN("execution request dropped (executor not implemented yet)");

		if (msg.payload != NULL) {
			k_heap_free(&wasp_payload_heap, msg.payload);
		}
	}
}

K_THREAD_DEFINE(wasp_exec_tid, CONFIG_WASP_EXEC_THREAD_STACK_SIZE, exec_thread_fn, NULL, NULL,
		NULL, K_PRIO_PREEMPT(7), 0, 0);
