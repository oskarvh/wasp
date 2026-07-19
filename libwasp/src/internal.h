/* libwasp internals shared between core.c and host.c. */
#ifndef WASP_INTERNAL_H_
#define WASP_INTERNAL_H_

#include <pthread.h>
#include <stdint.h>

#include "wasp/libwasp.h"

#define WASP_OWNER_MAX 32
#define WASP_DETAIL_MAX 128
#define WASP_RX_CAP (128 * 1024)
#define WASP_EVQ_CAP 8192

/* Timing policy, mirrors tools/wasp_client.py. */
#define WASP_LOCK_LEASE_MS 5000
/* Queued-lock park bound: must stay under the node's RPC timeout
 * (CONFIG_WASP_RPC_TIMEOUT_MS, 5 s) or the waiting module traps
 * instead of seeing a clean ELOCKED. */
#define WASP_LOCK_QUEUE_WAIT_MS 4000
#define WASP_ANNOUNCE_PORT 4243

struct wasp_region {
	uint8_t *buf;
	uint32_t len; /* 0 = not registered */
};

struct wasp_lock {
	char owner[WASP_OWNER_MAX];
	uint64_t expiry_ms;
	int held;
};

/* One parked LOCK request; lives on the waiting thread's stack. */
struct wasp_waiter {
	const char *owner;
	struct wasp_waiter *next;
};

struct wasp_ctx {
	/*
	 * Serializes all remote-memory servicing across every node on
	 * this ctx — IS the "coordinator serializes every access"
	 * guarantee. The condvar wakes queued-lock waiters on UNLOCK
	 * (waiting drops the mutex so the holder's own traffic can be
	 * serviced meanwhile).
	 */
	pthread_mutex_t mem_mu;
	pthread_cond_t mem_cv;
	struct wasp_region regions[256];
	struct wasp_lock locks[256];
	struct wasp_waiter *qhead[256], *qtail[256];

	pthread_mutex_t ev_mu;
	pthread_cond_t ev_cv;
	wasp_event evq[WASP_EVQ_CAP];
	uint32_t ev_head, ev_count;
	uint64_t ev_dropped;
};

struct wasp_node {
	wasp_ctx *ctx;
	int fd;
	uint8_t seq;
	uint32_t timeout_ms;
	char owner[WASP_OWNER_MAX];
	int queue_locks;
	uint32_t tag;
	uint32_t max_payload; /* from HELLO_ACK, 0 until then */
	uint8_t *rx;          /* WASP_RX_CAP; holds request + response */
	char detail[WASP_DETAIL_MAX];
};

uint64_t wasp_now_ms(void);
int wasp_send_frame(int fd, uint8_t type, uint8_t seq, const void *payload,
		    uint32_t len);

/* host.c: serve one node-initiated request (0x4x) and send the
 * response on n->fd. Returns 0 or WASP_E_* on transport failure. */
int wasp_host_serve(wasp_node *n, uint8_t rtype, uint8_t rseq,
		    uint8_t *payload, uint32_t len);

#endif /* WASP_INTERNAL_H_ */
