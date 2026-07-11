/*
 * Agent thread — the node's command dispatcher.
 *
 * Consumes coordinator commands from wasp_rx_q, manages WASM module
 * lifecycle through the wamr wrapper, hands execution requests to the
 * executor via wasp_exec_q, and emits responses on wasp_tx_q.
 *
 * Dispatch bodies are business logic and intentionally not implemented
 * yet — this is the skeleton the protocol work plugs into.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp_agent, LOG_LEVEL_INF);

static void handle_msg(struct wasp_msg *msg)
{
	switch (msg->type) {
	case WASP_MSG_LOAD_MODULE:
		/* TODO: wasp_wamr_load(msg->payload, msg->len) */
	case WASP_MSG_UNLOAD_MODULE:
		/* TODO: wasp_wamr_unload() */
	case WASP_MSG_CALL:
		/* TODO: forward to wasp_exec_q */
	case WASP_MSG_PING:
		/* TODO: PONG on wasp_tx_q */
	default:
		LOG_WRN("unhandled message type 0x%02x (dispatch not implemented yet)",
			msg->type);
		break;
	}

	if (msg->payload != NULL) {
		k_heap_free(&wasp_payload_heap, msg->payload);
	}
}

static void agent_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("agent ready, waiting for commands");

	while (true) {
		struct wasp_msg msg;

		/* Wake on either a coordinator command or an executor result.
		 * TODO: fold wasp_res_q in via k_poll once results exist. */
		if (k_msgq_get(&wasp_rx_q, &msg, K_FOREVER) == 0) {
			handle_msg(&msg);
		}
	}
}

K_THREAD_DEFINE(wasp_agent_tid, CONFIG_WASP_AGENT_THREAD_STACK_SIZE, agent_thread_fn, NULL, NULL,
		NULL, K_PRIO_PREEMPT(5), 0, 0);
