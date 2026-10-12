/*
 * wasp — WebAssembly Swarm Processor node.
 *
 * main() only owns the shared plumbing (queues, payload heap, tx helpers)
 * and a boot banner; the worker threads are defined in their own modules
 * and start via K_THREAD_DEFINE.
 */
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp, LOG_LEVEL_INF);

#ifdef CONFIG_WASP_WORKER

#define WASP_QUEUE_DEPTH 8

/*
 * Payload buffers travel through the queues by pointer; this heap backs
 * them. Sized to hold one max-size module (which stays allocated while
 * loaded) plus working room for control traffic.
 *
 * Placed in tightly-coupled memory when the board has it (CPU-only access
 * is fine — payloads are only ever memcpy'd). The WAMR pool deliberately
 * does NOT live in DTCM: it must satisfy a contiguous 64 KiB+ allocation
 * for a module's linear memory (one WASM page), which a 64 KiB TCM can
 * never fit alongside the runtime's own allocations, so it lives in main
 * SRAM and this heap takes the TCM instead.
 */
#define WASP_PAYLOAD_HEAP_SIZE (CONFIG_WASP_MAX_MODULE_SIZE + 8192)
#if DT_HAS_CHOSEN(zephyr_dtcm)
static uint8_t payload_heap_buf[WASP_PAYLOAD_HEAP_SIZE] __dtcm_noinit_section;
#else
static uint8_t payload_heap_buf[WASP_PAYLOAD_HEAP_SIZE] __noinit;
#endif
struct k_heap wasp_payload_heap;

static int wasp_payload_heap_init(void)
{
	k_heap_init(&wasp_payload_heap, payload_heap_buf, sizeof(payload_heap_buf));
	return 0;
}
/* POST_KERNEL: before the K_THREAD_DEFINE'd worker threads start. */
SYS_INIT(wasp_payload_heap_init, POST_KERNEL, 0);

K_MSGQ_DEFINE(wasp_rx_q, sizeof(struct wasp_msg), WASP_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(wasp_tx_q, sizeof(struct wasp_msg), WASP_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(wasp_exec_q, sizeof(struct wasp_msg), WASP_QUEUE_DEPTH, 4);

int wasp_queue_tx(const struct wasp_msg *req, uint8_t type, const void *payload, uint32_t len)
{
	struct wasp_msg msg = {
		.type = type,
		.seq = req->seq,
		.conn = req->conn,
		.len = len,
	};

	if (len > 0) {
		msg.payload = k_heap_alloc(&wasp_payload_heap, len, K_MSEC(100));
		if (msg.payload == NULL) {
			LOG_ERR("no memory for %u byte response, dropping", len);
			return -ENOMEM;
		}
		memcpy(msg.payload, payload, len);
	}

	if (k_msgq_put(&wasp_tx_q, &msg, K_MSEC(100)) != 0) {
		LOG_ERR("tx queue full, dropping response type 0x%02x", type);
		if (msg.payload != NULL) {
			k_heap_free(&wasp_payload_heap, msg.payload);
		}
		return -EAGAIN;
	}
	return 0;
}

int wasp_queue_error(const struct wasp_msg *req, uint8_t err, const char *detail)
{
	uint8_t buf[1 + 96];
	uint32_t len = 1;

	buf[0] = err;
	if (detail != NULL) {
		len += strlen(detail);
		len = MIN(len, sizeof(buf));
		memcpy(&buf[1], detail, len - 1);
	}
	return wasp_queue_tx(req, WASP_MSG_ERROR, buf, len);
}

#endif /* CONFIG_WASP_WORKER */

/*
 * Report the clock tree the SoC actually ended up with, not the one the
 * devicetree asked for. On the wasp node board these differ in an
 * interesting way: HSE is bypass-fed from the Ethernet switch's REFCLKO,
 * so "did the PLL lock onto a real 25 MHz?" is a board-level question,
 * and the answer is otherwise invisible — a silent fallback and a correct
 * boot look identical from the console.
 */
#if defined(CONFIG_SOC_FAMILY_STM32)
#include <stm32_ll_rcc.h>

static void log_clock_tree(void)
{
	static const char *const src[] = { "HSI", "CSI", "HSE", "PLL1" };
	uint32_t sws = LL_RCC_GetSysClkSource() >> RCC_CFGR_SWS_Pos;

	LOG_INF("sysclk %u MHz from %s (HSE %s, PLL1 %s), HCLK %u MHz",
		HAL_RCC_GetSysClockFreq() / 1000000U,
		src[sws & 0x3],
		LL_RCC_HSE_IsReady() ? "ready" : "off",
		LL_RCC_PLL1_IsReady() ? "locked" : "off",
		HAL_RCC_GetHCLKFreq() / 1000000U);
}
#else
static void log_clock_tree(void) { }
#endif

int main(void)
{
	LOG_INF("wasp node booting (board: %s)", CONFIG_BOARD);
	log_clock_tree();
	return 0;
}
