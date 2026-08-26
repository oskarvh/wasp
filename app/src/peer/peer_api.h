/*
 * The peer host-function API: everything a coordinator brain needs,
 * shaped so step 4 can register these functions as WAMR natives and
 * run the brain as a WASM module. Handles are small non-negative
 * ints; every function returns a wasp error code (< 0) on failure.
 *
 * One global peer context backs the API — regions exported with
 * wasp_export_region are served to every connected peer, and all
 * remote-memory servicing happens inline inside wasp_peer_call /
 * wasp_peer_poll on the calling thread (single-threaded conduction
 * is the intended model).
 */
#ifndef WASP_PEER_API_H_
#define WASP_PEER_API_H_

#include <stdint.h>

#include "wasp/libwasp.h"

/* Idempotent; called implicitly by wasp_peer_discover/connect. */
void wasp_peer_init(void);

/*
 * Listen wait_ms for worker ANNOUNCE broadcasts; returns how many
 * distinct workers were heard (this node's own addresses are
 * excluded). The list is kept until the next discover.
 */
int wasp_peer_discover(uint32_t wait_ms);
/* Discovered worker idx (0..count-1), or NULL out of range. */
const wasp_info *wasp_peer_info(int idx);

/* Dial discovered worker idx; returns a connection handle. */
int wasp_peer_connect(int idx);
/* Dial an explicit address (not everything announces). */
int wasp_peer_connect_addr(const char *ip, uint16_t port);
void wasp_peer_close(int h);
/* Async-safe: unblocks any request in flight on the handle. */
void wasp_peer_kill(int h);
const wasp_info *wasp_peer_conn_info(int h);

int wasp_peer_hello(int h, uint8_t *version, uint32_t *max_payload,
		    uint8_t *features);
int wasp_peer_load(int h, const void *wasm, uint32_t len);
int wasp_peer_unload(int h);
/* Blocking call; returns result count or negative error. */
int wasp_peer_call(int h, const char *fn, const int32_t *argv,
		   uint32_t argc, int32_t results[WASP_CALL_MAX_RESULTS]);
/* Async call + nonblocking completion poll (WASP_E_AGAIN while the
 * call runs; each poll also services the worker's wasp.* RPCs). */
int wasp_peer_call_async(int h, const char *fn, const int32_t *argv,
			 uint32_t argc);
int wasp_peer_poll(int h, int32_t results[WASP_CALL_MAX_RESULTS]);

/* Serve buf as region_id to every connected peer, from this node's
 * RAM (or, in step 4, from the brain module's linear memory). */
int wasp_export_region(uint8_t region_id, void *buf, uint32_t len);
int wasp_unexport_region(uint8_t region_id);

/* Observe peers' MEM_WRITEs into exported regions; ev->tag is the
 * writing connection's handle. 1 = event, 0 = timeout. */
int wasp_peer_poll_event(wasp_event *ev, uint32_t timeout_ms);

#endif /* WASP_PEER_API_H_ */
