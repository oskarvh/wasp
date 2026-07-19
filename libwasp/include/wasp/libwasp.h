/*
 * libwasp — the wasp coordinator as a C library.
 *
 * One wasp_ctx holds the coordinator-side state every connection
 * shares: the exported memory regions, the region locks (leases), and
 * an event queue. Each wasp_node is one TCP connection to a node.
 *
 * Threading model: a wasp_node is used by ONE thread at a time (give
 * each node its own thread for parallel work, like the Python tools
 * do). The ctx is fully thread-safe and is what serializes every
 * remote-memory access — that serialization is the atomicity
 * guarantee behind MEM_ADD/MEM_CAS. Requests block; node-initiated
 * remote-memory RPCs arriving while a request is in flight are
 * serviced inline from the caller's thread.
 *
 * Wire format and semantics: app/src/protocol.h (included below) is
 * the single source of truth; behavior mirrors tools/wasp_client.py.
 */
#ifndef WASP_LIBWASP_H_
#define WASP_LIBWASP_H_

#include <stdint.h>

#include "protocol.h" /* app/src/protocol.h */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return codes: 0 on success. -0x01..-0x0D is an ERROR frame from the
 * node, its wasp_err code negated (e.g. -WASP_ERR_TRAP); the detail
 * text is kept in wasp_node_error_detail(). -100 and below are local
 * failures.
 */
enum {
	WASP_OK = 0,
	WASP_E_TIMEOUT = -100,    /* node did not answer within the timeout */
	WASP_E_CONN = -101,       /* connect failed or connection closed */
	WASP_E_STREAM = -102,     /* bad magic / seq mismatch: stream unusable */
	WASP_E_ARG = -103,        /* invalid argument */
	WASP_E_TOOBIG = -104,     /* payload exceeds a buffer or the node's cap */
	WASP_E_SYS = -105,        /* OS-level failure, see errno */
	WASP_E_UNEXPECTED = -106, /* response type made no sense for the request */
};

const char *wasp_strerror(int err);

typedef struct wasp_ctx wasp_ctx;
typedef struct wasp_node wasp_node;

/* ---- context: shared regions, locks, events ------------------------ */

wasp_ctx *wasp_ctx_new(void);
void wasp_ctx_free(wasp_ctx *ctx);

/*
 * Export buf (len bytes, len < WASP_REF_MAX_OFFSET) as region_id to
 * every node on this ctx. The buffer stays owned by the caller and
 * must outlive the registration; re-registering an id replaces it.
 */
int wasp_region_register(wasp_ctx *ctx, uint8_t region_id, void *buf,
			 uint32_t len);
int wasp_region_unregister(wasp_ctx *ctx, uint8_t region_id);

/*
 * Lock administration, mostly for tests and tooling: plant a lease on
 * behalf of an (unconnected) owner, revoke one, query the holder.
 * wasp_lock_holder returns 1 and copies the holder name into out if
 * an unexpired lease exists, else 0.
 */
int wasp_lock_force(wasp_ctx *ctx, uint8_t region_id, const char *owner,
		    uint32_t lease_ms);
int wasp_lock_clear(wasp_ctx *ctx, uint8_t region_id);
int wasp_lock_holder(wasp_ctx *ctx, uint8_t region_id, char *out,
		     uint32_t cap);

/* ---- events -------------------------------------------------------- */

enum { WASP_EV_MEM_WRITE = 1 };

typedef struct {
	uint32_t tag;   /* wasp_node_set_tag() of the serving connection */
	uint8_t type;   /* WASP_EV_* */
	uint8_t region;
	uint32_t offset;
	uint32_t len;
} wasp_event;

/* 1 = event copied to out, 0 = timeout, <0 = error. */
int wasp_poll_event(wasp_ctx *ctx, wasp_event *out, uint32_t timeout_ms);

/* ---- nodes --------------------------------------------------------- */

wasp_node *wasp_connect(wasp_ctx *ctx, const char *host, uint16_t port,
			uint32_t timeout_ms);
void wasp_node_free(wasp_node *n);
/*
 * Shut the socket down without freeing: any thread blocked in a
 * request on this node fails with WASP_E_CONN. Safe to call from
 * another thread (wasp_node_free is not, while a request is running).
 */
void wasp_node_kill(wasp_node *n);

/* Lock-holder identity for this connection (default "node"); give
 * each node a distinct name when they share locked regions. */
void wasp_node_set_owner(wasp_node *n, const char *owner);
/* Queued-lock mode: park contended LOCKs FIFO-fair (bounded under the
 * node's RPC timeout) instead of failing fast with ERROR(LOCKED). */
void wasp_node_set_queue_locks(wasp_node *n, int on);
/* Opaque tag copied into events this connection generates. */
void wasp_node_set_tag(wasp_node *n, uint32_t tag);
/* Detail text of the last ERROR frame this node sent ("" if none). */
const char *wasp_node_error_detail(wasp_node *n);

/*
 * Send one frame, service node-initiated RPCs inline, return the
 * response. *rpayload points into the node's receive buffer — valid
 * until the next request on this node. Returns 0 even when the
 * response is an ERROR frame (raw API reports, wrappers interpret).
 */
int wasp_request(wasp_node *n, uint8_t type, const void *payload,
		 uint32_t len, uint8_t *rtype, uint8_t *rseq,
		 const uint8_t **rpayload, uint32_t *rlen);

int wasp_hello(wasp_node *n, uint8_t *version, uint32_t *max_payload,
	       uint8_t *features);
int wasp_load(wasp_node *n, const void *wasm, uint32_t len);
int wasp_unload(wasp_node *n);
/* Returns the number of i32 results (>= 0) or a negative error. */
int wasp_call(wasp_node *n, const char *fn, const int32_t *argv,
	      uint32_t argc, int32_t results[WASP_CALL_MAX_RESULTS]);

/* ---- discovery ----------------------------------------------------- */

typedef struct {
	char ip[46];
	uint16_t tcp_port;
	uint8_t version;
	uint8_t features;
	uint8_t busy;
	char board[33];
} wasp_info;

/* Listen for ANNOUNCE broadcasts; returns node count (deduped by IP)
 * or a negative error. Default timeout should cover one announce
 * interval (5 s). */
int wasp_discover(wasp_info *out, uint32_t max, uint32_t timeout_ms,
		  uint16_t udp_port);

static inline uint32_t wasp_pack_ref(uint8_t region_id, uint32_t offset)
{
	return ((uint32_t)region_id << 24) | (offset & 0x00FFFFFFu);
}

#ifdef __cplusplus
}
#endif

#endif /* WASP_LIBWASP_H_ */
