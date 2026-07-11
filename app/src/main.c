/*
 * wasp — WebAssembly Swarm Processor node.
 *
 * main() only owns the shared plumbing (queues, payload heap) and a boot
 * banner; the worker threads are defined in their own modules and start
 * via K_THREAD_DEFINE.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp, LOG_LEVEL_INF);

#define WASP_QUEUE_DEPTH 8

/* Payload buffers travel through the queues by pointer; this heap backs
 * them. Sized to hold one max-size module plus small control payloads. */
K_HEAP_DEFINE(wasp_payload_heap, CONFIG_WASP_MAX_MODULE_SIZE + 4096);

K_MSGQ_DEFINE(wasp_rx_q, sizeof(struct wasp_msg), WASP_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(wasp_tx_q, sizeof(struct wasp_msg), WASP_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(wasp_exec_q, sizeof(struct wasp_msg), WASP_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(wasp_res_q, sizeof(struct wasp_msg), WASP_QUEUE_DEPTH, 4);

int main(void)
{
	LOG_INF("wasp node booting (board: %s)", CONFIG_BOARD);
	return 0;
}
