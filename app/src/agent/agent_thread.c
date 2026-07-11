/*
 * Agent thread — the node's command dispatcher.
 *
 * Consumes coordinator commands from wasp_rx_q and answers on wasp_tx_q.
 * Protocol-level commands (HELLO, PING) are handled here; WASM lifecycle
 * commands will be routed to the wamr wrapper / executor once that layer
 * is implemented — until then they are answered with ERR_UNSUPPORTED.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp_agent, LOG_LEVEL_INF);

/*
 * Queue a response frame. The payload (if any) is copied into a fresh
 * heap allocation owned by the tx queue consumer (the net thread).
 */
static void reply(uint8_t type, uint8_t seq, const uint8_t *payload, uint32_t len)
{
	struct wasp_msg msg = {
		.type = type,
		.seq = seq,
		.len = len,
	};

	if (len > 0) {
		msg.payload = k_heap_alloc(&wasp_payload_heap, len, K_MSEC(100));
		if (msg.payload == NULL) {
			LOG_ERR("no memory for %u byte response, dropping", len);
			return;
		}
		memcpy(msg.payload, payload, len);
	}

	if (k_msgq_put(&wasp_tx_q, &msg, K_MSEC(100)) != 0) {
		LOG_ERR("tx queue full, dropping response type 0x%02x", type);
		if (msg.payload != NULL) {
			k_heap_free(&wasp_payload_heap, msg.payload);
		}
	}
}

static void reply_error(uint8_t seq, uint8_t err)
{
	reply(WASP_MSG_ERROR, seq, &err, 1);
}

static void handle_hello(const struct wasp_msg *msg)
{
	uint8_t ack[5];

	if (msg->len >= 1 && msg->payload[0] != WASP_PROTO_VERSION) {
		LOG_WRN("coordinator speaks protocol v%u, node speaks v%u", msg->payload[0],
			WASP_PROTO_VERSION);
	}

	ack[0] = WASP_PROTO_VERSION;
	sys_put_le32(CONFIG_WASP_MAX_MODULE_SIZE, &ack[1]);
	reply(WASP_MSG_HELLO_ACK, msg->seq, ack, sizeof(ack));
	LOG_INF("coordinator handshake complete");
}

static void handle_msg(struct wasp_msg *msg)
{
	switch (msg->type) {
	case WASP_MSG_HELLO:
		handle_hello(msg);
		break;
	case WASP_MSG_PING:
		reply(WASP_MSG_PONG, msg->seq, msg->payload, msg->len);
		break;
	case WASP_MSG_LOAD_MODULE:
	case WASP_MSG_UNLOAD_MODULE:
	case WASP_MSG_CALL:
		/* TODO(lifecycle): route to the wamr wrapper / wasp_exec_q. */
		LOG_WRN("type 0x%02x not implemented yet (WAMR lifecycle)", msg->type);
		reply_error(msg->seq, WASP_ERR_UNSUPPORTED);
		break;
	default:
		LOG_WRN("unknown message type 0x%02x", msg->type);
		reply_error(msg->seq, WASP_ERR_BAD_TYPE);
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

		/* TODO(lifecycle): fold wasp_res_q in via k_poll once the
		 * executor produces results. */
		if (k_msgq_get(&wasp_rx_q, &msg, K_FOREVER) == 0) {
			handle_msg(&msg);
		}
	}
}

K_THREAD_DEFINE(wasp_agent_tid, CONFIG_WASP_AGENT_THREAD_STACK_SIZE, agent_thread_fn, NULL, NULL,
		NULL, K_PRIO_PREEMPT(5), 0, 0);
