/*
 * wasp — shared definitions for inter-thread messaging.
 *
 * Thread/queue topology (see README.md):
 *
 *   net_thread --rx_q--> agent_thread --exec_q--> exec_thread
 *   net_thread <--tx_q-- agent_thread <--res_q--- exec_thread
 */
#ifndef WASP_H_
#define WASP_H_

#include <stdint.h>
#include <zephyr/kernel.h>

/* Wire protocol frame types (draft — see README.md). */
enum wasp_msg_type {
	WASP_MSG_HELLO = 0x01,
	WASP_MSG_HELLO_ACK = 0x02,
	WASP_MSG_LOAD_MODULE = 0x03,
	WASP_MSG_UNLOAD_MODULE = 0x04,
	WASP_MSG_CALL = 0x05,
	WASP_MSG_RESULT = 0x06,
	WASP_MSG_ERROR = 0x07,
	WASP_MSG_PING = 0x08,
	WASP_MSG_PONG = 0x09,
};

/*
 * A message passed between threads. The payload is allocated from
 * wasp_payload_heap by the producer and must be freed by the final
 * consumer.
 */
struct wasp_msg {
	uint8_t type; /* enum wasp_msg_type */
	uint8_t seq;
	uint32_t len;
	uint8_t *payload;
};

extern struct k_heap wasp_payload_heap;

extern struct k_msgq wasp_rx_q;   /* net -> agent: commands from coordinator */
extern struct k_msgq wasp_tx_q;   /* agent -> net: responses to coordinator */
extern struct k_msgq wasp_exec_q; /* agent -> executor: run requests */
extern struct k_msgq wasp_res_q;  /* executor -> agent: run outcomes */

#endif /* WASP_H_ */
