/*
 * libwasp internals shared between core.c and host.c — and included
 * by embedders (node firmware) that need the struct sizes for static
 * allocation and the no-malloc init API at the bottom.
 */
#ifndef WASP_INTERNAL_H_
#define WASP_INTERNAL_H_

#include <stdint.h>

#include "port.h"
#include "wasp/libwasp.h"

/*
 * Compile-time sizing. POSIX defaults are generous; firmware builds
 * override via compile definitions (see app CMakeLists):
 *   WASP_MAX_REGIONS  region ids >= this answer NO_REGION (tables are
 *                     dimensioned by it: ~70 B per region)
 *   WASP_EVQ_CAP      event queue entries (16 B each)
 */
#ifndef WASP_MAX_REGIONS
#define WASP_MAX_REGIONS 256
#endif
#ifndef WASP_EVQ_CAP
#define WASP_EVQ_CAP 8192
#endif
/*   WASP_MAX_INFLIGHT  concurrent async calls per node (~24 B each). A
 *                      node executes one call per instance at a time, so
 *                      there is no point exceeding its instance count. */
#ifndef WASP_MAX_INFLIGHT
#define WASP_MAX_INFLIGHT 8
#endif
/* Default rx buffer for the malloc-based wasp_connect (POSIX). */
#ifndef WASP_RX_CAP_DEFAULT
#define WASP_RX_CAP_DEFAULT (128 * 1024)
#endif

#define WASP_OWNER_MAX 32
#define WASP_DETAIL_MAX 128

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
	wasp_mutex_t mem_mu;
	wasp_cond_t mem_cv;
	struct wasp_region regions[WASP_MAX_REGIONS];
	struct wasp_lock locks[WASP_MAX_REGIONS];
	struct wasp_waiter *qhead[WASP_MAX_REGIONS];
	struct wasp_waiter *qtail[WASP_MAX_REGIONS];

	wasp_mutex_t ev_mu;
	wasp_cond_t ev_cv;
	wasp_event evq[WASP_EVQ_CAP];
	uint32_t ev_head, ev_count;
	uint64_t ev_dropped;
};

struct wasp_node {
	wasp_ctx *ctx;
	int fd;
	uint8_t seq;
	int heap; /* allocated by wasp_connect -> wasp_node_free frees it */
	uint32_t timeout_ms;
	char owner[WASP_OWNER_MAX];
	int queue_locks;
	uint32_t tag;
	uint32_t max_payload; /* from HELLO_ACK, 0 until then */
	uint8_t *rx;          /* rx_cap bytes; holds request + response */
	uint32_t rx_cap;
	char detail[WASP_DETAIL_MAX];

	/*
	 * Outstanding async calls. There used to be exactly one, on the
	 * grounds that the node's executor was single-threaded; it now runs
	 * a thread per instance, so several calls to different instances
	 * really do proceed at once and the coordinator has to be able to
	 * have several in flight to use that.
	 *
	 * A response is parsed into its slot the moment it arrives, rather
	 * than left in n->rx for the poller to read later: rx holds one
	 * frame, and with several calls outstanding the next frame would
	 * overwrite it before anyone harvested the result.
	 */
	struct wasp_call_slot {
		int in_use; /* sent, not yet harvested */
		int done;   /* response parsed into rc/results */
		int rc;     /* wasp_call's return convention */
		uint8_t seq;
		uint8_t inst;
		int has_inst; /* CALL_INST rather than the v1 CALL */
		int32_t results[WASP_CALL_MAX_RESULTS];
	} calls[WASP_MAX_INFLIGHT];
};

/*
 * Static-allocation API for embedders: no malloc anywhere. The POSIX
 * conveniences (wasp_ctx_new/wasp_connect) wrap these.
 */
void wasp_ctx_init(wasp_ctx *ctx);
/* Adopt an already-connected fd (from wasp_net_connect). */
void wasp_node_attach(wasp_node *n, wasp_ctx *ctx, int fd, uint8_t *rx,
		      uint32_t rx_cap, uint32_t timeout_ms);
/* Connect + attach; WASP_OK or WASP_E_CONN. */
int wasp_node_connect_static(wasp_node *n, wasp_ctx *ctx, uint8_t *rx,
			     uint32_t rx_cap, const char *host, uint16_t port,
			     uint32_t timeout_ms);

int wasp_send_frame(int fd, uint8_t type, uint8_t seq, const void *payload,
		    uint32_t len);

/* host.c: serve one node-initiated request (0x4x) and send the
 * response on n->fd. Returns 0 or WASP_E_* on transport failure. */
int wasp_host_serve(wasp_node *n, uint8_t rtype, uint8_t rseq,
		    uint8_t *payload, uint32_t len);

#endif /* WASP_INTERNAL_H_ */
