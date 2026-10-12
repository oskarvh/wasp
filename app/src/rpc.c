/*
 * Node -> coordinator RPC slots for remote memory.
 *
 * One slot per instance that can have a call in flight. Each caller
 * blocks on its own semaphore, so an instance parked on a remote-memory
 * round trip does not hold up the others — which is the whole reason the
 * executor is a pool rather than one thread.
 *
 * Responses are matched on the node's own seq. The seq counter skips
 * values that are currently outstanding: it is a u8, a round trip has a
 * ~2.8 ms floor on Ethernet and a 5 s timeout, so 256 RPCs can comfortably
 * be issued while one is still parked and a naive counter would eventually
 * hand a second caller the first caller's seq.
 *
 * One spinlock covers the whole table. It is held only for a few field
 * assignments, never across a send or a wait, and it also arbitrates the
 * tiny window where a response and a caller timeout race.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp_rpc, LOG_LEVEL_INF);

struct rpc_slot {
	struct k_sem done;
	bool waiting;   /* claimed, response not yet delivered */
	bool delivered; /* resp holds a consumed-but-not-collected response */
	uint8_t seq;
	struct wasp_msg resp;
};

static struct k_spinlock rpc_lock;
static struct rpc_slot slots[CONFIG_WASP_MAX_INSTANCES];

/* Node-side seq space, independent of the coordinator's request seqs. */
static uint8_t node_seq;

static int rpc_init(void)
{
	for (int i = 0; i < ARRAY_SIZE(slots); i++) {
		k_sem_init(&slots[i].done, 0, 1);
	}
	return 0;
}
SYS_INIT(rpc_init, POST_KERNEL, 0);

/* Caller must hold rpc_lock. Returns a seq no outstanding slot is using. */
static uint8_t next_free_seq(void)
{
	for (int tries = 0; tries < 256; tries++) {
		uint8_t cand = ++node_seq;
		bool taken = false;

		for (int i = 0; i < ARRAY_SIZE(slots); i++) {
			if (slots[i].waiting && slots[i].seq == cand) {
				taken = true;
				break;
			}
		}
		if (!taken) {
			return cand;
		}
	}
	return ++node_seq; /* unreachable with < 256 slots */
}

int wasp_rpc_call(uint8_t type, const void *payload, uint32_t len, uint8_t conn,
		  struct wasp_msg *resp, k_timeout_t timeout)
{
	struct wasp_msg req = {
		.type = type,
		.conn = conn,
		.len = len,
	};
	struct rpc_slot *slot = NULL;
	k_spinlock_key_t key;

	if (len > 0) {
		req.payload = k_heap_alloc(&wasp_payload_heap, len, K_MSEC(100));
		if (req.payload == NULL) {
			LOG_ERR("no memory for %u byte RPC request", len);
			return -ENOMEM;
		}
		memcpy(req.payload, payload, len);
	}

	key = k_spin_lock(&rpc_lock);
	for (int i = 0; i < ARRAY_SIZE(slots); i++) {
		if (!slots[i].waiting && !slots[i].delivered) {
			slot = &slots[i];
			break;
		}
	}
	if (slot != NULL) {
		req.seq = next_free_seq();
		slot->waiting = true;
		slot->delivered = false;
		slot->seq = req.seq;
	}
	k_spin_unlock(&rpc_lock, key);

	if (slot == NULL) {
		/* Can only happen if more callers exist than instances, which
		 * the executor pool prevents by construction.
		 */
		LOG_ERR("no free RPC slot for type 0x%02x", type);
		if (req.payload != NULL) {
			k_heap_free(&wasp_payload_heap, req.payload);
		}
		return -EAGAIN;
	}

	if (k_msgq_put(&wasp_tx_q, &req, K_MSEC(100)) != 0) {
		LOG_ERR("tx queue full, dropping RPC type 0x%02x", type);
		if (req.payload != NULL) {
			k_heap_free(&wasp_payload_heap, req.payload);
		}
		key = k_spin_lock(&rpc_lock);
		slot->waiting = false;
		k_spin_unlock(&rpc_lock, key);
		return -EAGAIN;
	}

	if (k_sem_take(&slot->done, timeout) == 0) {
		key = k_spin_lock(&rpc_lock);
		*resp = slot->resp;
		slot->delivered = false;
		k_spin_unlock(&rpc_lock, key);
		return 0;
	}

	/* Timed out — but the response may have landed in the gap. */
	key = k_spin_lock(&rpc_lock);
	slot->waiting = false;
	if (slot->delivered) {
		*resp = slot->resp;
		slot->delivered = false;
		k_sem_reset(&slot->done);
		k_spin_unlock(&rpc_lock, key);
		return 0;
	}
	k_spin_unlock(&rpc_lock, key);
	LOG_WRN("RPC type 0x%02x seq %u timed out", type, req.seq);
	return -ETIMEDOUT;
}

bool wasp_rpc_deliver(struct wasp_msg *msg)
{
	k_spinlock_key_t key = k_spin_lock(&rpc_lock);

	for (int i = 0; i < ARRAY_SIZE(slots); i++) {
		struct rpc_slot *slot = &slots[i];

		if (!slot->waiting || slot->seq != msg->seq) {
			continue;
		}
		slot->waiting = false;
		slot->delivered = true;
		slot->resp = *msg;
		k_spin_unlock(&rpc_lock, key);
		k_sem_give(&slot->done);
		return true;
	}
	k_spin_unlock(&rpc_lock, key);
	return false;
}
