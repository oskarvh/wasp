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

#include "protocol.h"

/*
 * A message passed between threads. The payload is allocated from
 * wasp_payload_heap by the producer and must be freed by the final
 * consumer.
 *
 * conn is not part of the wire format: the net thread stamps each rx
 * message with the current connection generation, replies inherit it,
 * and the net thread discards any tx message whose generation does not
 * match the live connection — a late response (e.g. produced after a
 * coordinator gave up and reconnected) must never reach the wrong peer.
 */
struct wasp_msg {
	uint8_t type; /* enum wasp_msg_type */
	uint8_t seq;
	uint8_t conn;
	uint32_t len;
	uint8_t *payload;
};

extern struct k_heap wasp_payload_heap;

extern struct k_msgq wasp_rx_q;   /* net -> agent: commands from coordinator */
extern struct k_msgq wasp_tx_q;   /* agent/executor -> net: responses to coordinator */
extern struct k_msgq wasp_exec_q; /* agent -> executor: lifecycle + call requests */

/*
 * Queue a response to req on wasp_tx_q (seq and connection generation
 * are taken from the request). The payload (may be NULL when len == 0)
 * is copied into a fresh heap allocation owned by the tx queue consumer.
 * Returns 0, or a negative errno when the response had to be dropped
 * (already logged).
 */
int wasp_queue_tx(const struct wasp_msg *req, uint8_t type, const void *payload, uint32_t len);

/* Queue an ERROR response to req: [code u8][optional utf8 detail]. */
int wasp_queue_error(const struct wasp_msg *req, uint8_t err, const char *detail);

/*
 * Node-initiated RPC to the coordinator (remote memory). Only the
 * executor thread may call wasp_rpc_call — it is the one place a
 * request can originate (inside a host function, mid-CALL), so a single
 * response slot suffices. Sends a frame with the node's own seq space,
 * stamped with connection generation conn, and blocks until the
 * coordinator's response arrives or timeout expires.
 *
 * On 0, *resp holds the response (type/len/payload; payload from
 * wasp_payload_heap, freed by the caller). -ETIMEDOUT or -ENOMEM/-EAGAIN
 * on send failure.
 */
int wasp_rpc_call(uint8_t type, const void *payload, uint32_t len, uint8_t conn,
		  struct wasp_msg *resp, k_timeout_t timeout);

/*
 * Called by the agent for inbound coordinator->node response frames
 * (MEM_DATA, MEM_ACK, LOCK_GRANT, REGION_DESC, ERROR). Returns true if
 * the message matched the outstanding RPC and was consumed (payload
 * ownership transferred); false if no RPC wants it (caller keeps
 * ownership).
 */
bool wasp_rpc_deliver(struct wasp_msg *msg);

#endif /* WASP_H_ */
