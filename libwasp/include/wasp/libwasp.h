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
 * Return codes: 0 on success. -0x01..-0x0F is an ERROR frame from the
 * node, its wasp_err code negated (e.g. -WASP_ERR_TRAP, or
 * -WASP_ERR_BAD_HANDLE for a retired instance handle); the detail text is
 * kept in wasp_node_error_detail(). -100 and below are local failures.
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
	WASP_E_AGAIN = -107,      /* async call still running, poll again */
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

/* ---- several instances per node (WASP_FEAT_MULTI_INSTANCE) ---------
 *
 * A module is validated bytecode; an instance is an instantiation of it
 * with its own linear memory. Splitting them is what lets one node host
 * unrelated programs, and what makes "same bytecode, several instances"
 * expressible: wasp_upload's n_inst sends the binary once and returns
 * that many instance handles in a single round trip.
 *
 * Handles carry a generation, so a handle to a dropped instance is
 * refused (WASP_E_* from ERROR(BAD_HANDLE)) rather than reaching
 * whatever later took its slot.
 *
 * Nodes that do not advertise WASP_FEAT_MULTI_INSTANCE still work
 * through wasp_load/wasp_call, which address one implicit instance.
 */

/* Upload bytecode and instantiate it n_inst times (0 = upload only).
 * Writes the module handle to *mod_out and the instance handles to
 * inst_out; returns the number of instances created or a negative error. */
int wasp_upload(wasp_node *n, const void *wasm, uint32_t len, uint8_t n_inst,
		uint8_t *mod_out, uint8_t *inst_out, uint8_t inst_cap);

/* More instances of bytecode the node already holds; nothing re-sent. */
int wasp_instantiate(wasp_node *n, uint8_t mod, uint8_t count,
		     uint8_t *inst_out, uint8_t inst_cap);

/* Returns the number of i32 results (>= 0) or a negative error. */
int wasp_call_inst(wasp_node *n, uint8_t inst, const char *fn,
		   const int32_t *argv, uint32_t argc,
		   int32_t results[WASP_CALL_MAX_RESULTS]);

int wasp_drop_instance(wasp_node *n, uint8_t inst);
/* Drops the bytecode and every instance of it. */
int wasp_drop_module(wasp_node *n, uint8_t mod);

typedef struct {
	uint8_t slots_total;
	uint8_t slots_free;
	uint32_t pool_free; /* bytes the node's WAMR pool has left */
	uint8_t n_modules;
	struct {
		uint8_t mod;
		uint8_t n_inst;
		uint8_t inst[WASP_HANDLE_SLOTS];
	} modules[WASP_HANDLE_SLOTS];
} wasp_inventory;

int wasp_inventory_get(wasp_node *n, wasp_inventory *out);

/*
 * Async calls: send and return immediately, then poll. wasp_call_poll
 * drains the connection without blocking, services the node's
 * remote-memory RPCs inline and, once a RESULT or ERROR arrives,
 * completes with wasp_call's return convention; while nothing has
 * completed it returns WASP_E_AGAIN. Interleave polls across nodes to
 * conduct several workers from one thread.
 *
 * Up to WASP_MAX_INFLIGHT calls may be outstanding per node, one per
 * instance — a node runs one call per instance at a time and answers
 * ERROR(BUSY) to a second on the same one. wasp_call_poll_any completes
 * whichever responds first and reports which instance that was, which is
 * the point of having several in flight: a call parked on a
 * remote-memory round trip does not hold up the others.
 *
 * It is safe to issue a synchronous request while async calls are
 * outstanding; their responses are parsed as they arrive and kept until
 * polled.
 */
int wasp_call_async(wasp_node *n, const char *fn, const int32_t *argv,
		    uint32_t argc);
int wasp_call_inst_async(wasp_node *n, uint8_t inst, const char *fn,
			 const int32_t *argv, uint32_t argc);
int wasp_call_poll(wasp_node *n, int32_t results[WASP_CALL_MAX_RESULTS]);
int wasp_call_poll_any(wasp_node *n, uint8_t *inst_out,
		       int32_t results[WASP_CALL_MAX_RESULTS]);
/* Async calls sent but not yet harvested. */
int wasp_call_inflight(wasp_node *n);

/* ---- discovery ----------------------------------------------------- */

typedef struct {
	char ip[46];
	uint16_t tcp_port;
	uint8_t version;
	uint8_t features;
	uint8_t busy;
	char board[33];
	/*
	 * Capacity, from the optional tail of the ANNOUNCE datagram. Zero
	 * when the node predates it. Worth having because `busy` is too
	 * coarse once a node has several instance slots, and because the
	 * node's WAMR pool is sized at boot from whatever RAM that board
	 * had spare — so how much it can host is a property of the node,
	 * and a job can be sized before connecting.
	 */
	uint8_t slots_total;
	uint8_t slots_free;
	uint32_t pool_free;
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
