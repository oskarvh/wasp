/*
 * Agent thread — the node's command dispatcher.
 *
 * Consumes coordinator commands from wasp_rx_q. Protocol-level commands
 * (HELLO, PING) are answered here; WASM lifecycle commands (LOAD_MODULE,
 * UNLOAD_MODULE, CALL) are routed to the executor thread via wasp_exec_q,
 * which serializes all runtime access and answers on wasp_tx_q itself.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#ifdef CONFIG_SOC_RP2040
#include <pico/bootrom.h>
#endif

#include "led.h"
#include "wasp.h"

LOG_MODULE_REGISTER(wasp_agent, LOG_LEVEL_INF);

static void handle_hello(const struct wasp_msg *msg)
{
	uint8_t ack[6];

	if (msg->len >= 1 && msg->payload[0] != WASP_PROTO_VERSION) {
		LOG_WRN("coordinator speaks protocol v%u, node speaks v%u", msg->payload[0],
			WASP_PROTO_VERSION);
	}

	ack[0] = WASP_PROTO_VERSION;
	sys_put_le32(CONFIG_WASP_MAX_MODULE_SIZE, &ack[1]);
	ack[5] = WASP_FEAT_REMOTE_MEM;
	wasp_queue_tx(msg, WASP_MSG_HELLO_ACK, ack, sizeof(ack));
	LOG_INF("coordinator handshake complete");
}

static void handle_reboot(const struct wasp_msg *msg)
{
	uint8_t mode = msg->len >= 1 ? msg->payload[0] : 0;

#ifndef CONFIG_SOC_RP2040
	if (mode == 1) {
		wasp_queue_error(msg, WASP_ERR_UNSUPPORTED, "no bootloader entry on this SoC");
		return;
	}
#endif

	LOG_INF("rebooting%s on coordinator request", mode == 1 ? " into USB bootloader" : "");
	wasp_queue_tx(msg, WASP_MSG_RESULT, NULL, 0);
	/* Let the net thread flush the RESULT before we pull the plug. */
	k_sleep(K_MSEC(500));

#ifdef CONFIG_SOC_RP2040
	if (mode == 1) {
		reset_usb_boot(0, 0);
	}
#endif
	sys_reboot(SYS_REBOOT_COLD);
}

static void handle_msg(struct wasp_msg *msg)
{
	switch (msg->type) {
	case WASP_MSG_HELLO:
		handle_hello(msg);
		break;
	case WASP_MSG_PING:
		wasp_queue_tx(msg, WASP_MSG_PONG, msg->payload, msg->len);
		break;
	case WASP_MSG_IDENTIFY:
		wasp_led_identify((msg->len >= 1 ? msg->payload[0] : 10) * 1000);
		wasp_queue_tx(msg, WASP_MSG_RESULT, NULL, 0);
		break;
	case WASP_MSG_REBOOT:
		handle_reboot(msg);
		break;
	case WASP_MSG_LOAD_MODULE:
	case WASP_MSG_UNLOAD_MODULE:
	case WASP_MSG_CALL:
		/* Hand the whole message (payload ownership included) to the
		 * executor; it answers the coordinator itself. */
		if (k_msgq_put(&wasp_exec_q, msg, K_MSEC(100)) != 0) {
			LOG_WRN("executor queue full, rejecting type 0x%02x", msg->type);
			wasp_queue_error(msg, WASP_ERR_BUSY, "executor busy");
			break;
		}
		return; /* payload now owned by the executor */
	case WASP_MSG_MEM_DATA:
	case WASP_MSG_MEM_ACK:
	case WASP_MSG_LOCK_GRANT:
	case WASP_MSG_REGION_DESC:
	case WASP_MSG_ERROR:
		/* Coordinator's answer to a node-initiated RPC. */
		if (wasp_rpc_deliver(msg)) {
			return; /* payload now owned by the RPC caller */
		}
		LOG_WRN("unsolicited response type 0x%02x seq %u, dropping", msg->type,
			msg->seq);
		break;
	default:
		LOG_WRN("unknown message type 0x%02x", msg->type);
		wasp_queue_error(msg, WASP_ERR_BAD_TYPE, NULL);
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

		if (k_msgq_get(&wasp_rx_q, &msg, K_FOREVER) == 0) {
			handle_msg(&msg);
		}
	}
}

K_THREAD_DEFINE(wasp_agent_tid, CONFIG_WASP_AGENT_THREAD_STACK_SIZE, agent_thread_fn, NULL, NULL,
		NULL, K_PRIO_PREEMPT(5), 0, 0);
