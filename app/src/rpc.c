/*
 * Node -> coordinator RPC slot for remote memory.
 *
 * Exactly one RPC can be outstanding: only the executor thread issues
 * them (from inside a host function, mid-CALL) and it blocks until the
 * response arrives. The agent thread delivers responses from the rx
 * path; a spinlock arbitrates the tiny window where a response and a
 * caller timeout race.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp_rpc, LOG_LEVEL_INF);

static struct {
	struct k_spinlock lock;
	struct k_sem done;
	bool waiting;   /* an RPC is outstanding, response not yet delivered */
	bool delivered; /* resp holds a consumed-but-not-collected response */
	uint8_t seq;
	struct wasp_msg resp;
} rpc = {
	.done = Z_SEM_INITIALIZER(rpc.done, 0, 1),
};

/* Node-side seq space, independent of the coordinator's request seqs. */
static uint8_t node_seq;

int wasp_rpc_call(uint8_t type, const void *payload, uint32_t len, uint8_t conn,
		  struct wasp_msg *resp, k_timeout_t timeout)
{
	struct wasp_msg req = {
		.type = type,
		.seq = ++node_seq,
		.conn = conn,
		.len = len,
	};
	k_spinlock_key_t key;

	if (len > 0) {
		req.payload = k_heap_alloc(&wasp_payload_heap, len, K_MSEC(100));
		if (req.payload == NULL) {
			LOG_ERR("no memory for %u byte RPC request", len);
			return -ENOMEM;
		}
		memcpy(req.payload, payload, len);
	}

	key = k_spin_lock(&rpc.lock);
	rpc.waiting = true;
	rpc.delivered = false;
	rpc.seq = req.seq;
	k_spin_unlock(&rpc.lock, key);

	if (k_msgq_put(&wasp_tx_q, &req, K_MSEC(100)) != 0) {
		LOG_ERR("tx queue full, dropping RPC type 0x%02x", type);
		if (req.payload != NULL) {
			k_heap_free(&wasp_payload_heap, req.payload);
		}
		key = k_spin_lock(&rpc.lock);
		rpc.waiting = false;
		k_spin_unlock(&rpc.lock, key);
		return -EAGAIN;
	}

	if (k_sem_take(&rpc.done, timeout) == 0) {
		key = k_spin_lock(&rpc.lock);
		*resp = rpc.resp;
		rpc.delivered = false;
		k_spin_unlock(&rpc.lock, key);
		return 0;
	}

	/* Timed out — but the response may have landed in the gap. */
	key = k_spin_lock(&rpc.lock);
	rpc.waiting = false;
	if (rpc.delivered) {
		*resp = rpc.resp;
		rpc.delivered = false;
		k_sem_reset(&rpc.done);
		k_spin_unlock(&rpc.lock, key);
		return 0;
	}
	k_spin_unlock(&rpc.lock, key);
	LOG_WRN("RPC type 0x%02x seq %u timed out", type, req.seq);
	return -ETIMEDOUT;
}

bool wasp_rpc_deliver(struct wasp_msg *msg)
{
	k_spinlock_key_t key = k_spin_lock(&rpc.lock);

	if (!rpc.waiting || msg->seq != rpc.seq) {
		k_spin_unlock(&rpc.lock, key);
		return false;
	}
	rpc.waiting = false;
	rpc.delivered = true;
	rpc.resp = *msg;
	k_spin_unlock(&rpc.lock, key);
	k_sem_give(&rpc.done);
	return true;
}
