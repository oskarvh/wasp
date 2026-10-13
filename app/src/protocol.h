/*
 * wasp wire protocol — single source of truth for the frame format.
 * Keep in sync with tools/wasp_client.py (and eventually the coordinator).
 *
 * All frames, both directions:
 *
 *   ┌───────────┬──────────┬─────────┬────────────┬───────────┐
 *   │ 'W' 'A'   │ type u8  │ seq u8  │ len u32 LE │ payload   │
 *   └───────────┴──────────┴─────────┴────────────┴───────────┘
 *
 * Responses echo the seq of the request they answer. Payload length is
 * capped at CONFIG_WASP_MAX_MODULE_SIZE (advertised in HELLO_ACK).
 *
 * Payloads:
 *   HELLO      (c->n): [proto_version u8]
 *   HELLO_ACK  (n->c): [proto_version u8][max_payload u32 LE][features u8]
 *   LOAD_MODULE(c->n): [.wasm binary]; success ack is an empty RESULT
 *   UNLOAD_MODULE (c->n): empty; success ack is an empty RESULT
 *   CALL       (c->n): [name_len u8][name][argc u8][arg u32 LE × argc]
 *                      v1 calling convention: i32 args/results only
 *
 * The three above address "the" module, of which there is one. They are
 * kept exactly as they were and are now sugar for the default instance,
 * so every existing client keeps working untouched. Nodes that can host
 * more than one module advertise WASP_FEAT_MULTI_INSTANCE and also accept:
 *
 *   MODULE_UPLOAD (c->n): [n_inst u8][.wasm binary]
 *                 (n->c): RESULT [mod u8][n u8][inst u8 × n]
 *   INSTANTIATE   (c->n): [mod u8][count u8]
 *                 (n->c): RESULT [count u8][inst u8 × count]
 *   CALL_INST     (c->n): [inst u8][name_len u8][name][argc u8][arg u32 × argc]
 *                 (n->c): RESULT, as CALL
 *   DROP          (c->n): [kind u8][handle u8]
 *                         kind 0 = instance, 1 = module (and its instances)
 *                 (n->c): RESULT, empty
 *   STATS         (c->n): [reset u8: non-zero clears the counters after
 *                         reporting]
 *                 (n->c): RESULT [n u8][{inst u8, calls u32, traps u32,
 *                         exec_us u32, rpc_count u32, rpc_wait_us u32,
 *                         rpc_tx u32, rpc_rx u32} x n]  (all LE)
 *
 * STATS is per instance because that is the grain the interesting
 * question has: how much of a call went into the interpreter and how much
 * into waiting for the coordinator. Instances of one node interleave
 * precisely so one computes while another waits, so a node-wide total
 * hides it. rpc_wait_us is accrued *inside* exec_us, which is what makes
 * the split arithmetic work:
 *
 *     compute  = exec_us - rpc_wait_us
 *     waiting  = rpc_wait_us / exec_us
 *
 * Microseconds, not nanoseconds, so a u32 covers ~71 minutes of
 * accumulated time per counter.
 *
 * The last row always carries inst = 0xFF: the totals of instances that
 * have since been dropped. Without it, "run a workload then ask what
 * happened" reports nothing, because the counters live on the instance
 * and releasing the module is the last thing a client does. 0xFF can
 * never name a live instance, the table being far smaller than the
 * sixteen slots a handle's index field could address.
 *
 *   INVENTORY     (c->n): empty
 *                 (n->c): RESULT [slots_total u8][slots_free u8]
 *                         [pool_free u32 LE][n_mod u8]
 *                         [{mod u8, n_inst u8, inst u8 × n_inst} × n_mod]
 *
 * The split is WAMR's own: a *module* is validated bytecode
 * (wasm_module_t), an *instance* is an instantiation of it with its own
 * linear memory (wasm_module_inst_t). Separating them is what lets the
 * same bytecode back several instances and different bytecode run side by
 * side, and it is why MODULE_UPLOAD carries a count: n_inst = 3 sends the
 * binary once and returns three instance handles in one round trip, while
 * n_inst = 0 uploads without instantiating. INSTANTIATE adds instances to
 * bytecode the node already holds, with nothing re-sent. Instances are
 * independent — nothing ties a node's instances to one program.
 *
 * Handles are (gen:4 | idx:4). The generation is not decoration: without
 * it, dropping instance 1 and instantiating into the freed slot would make
 * a stale CALL_INST execute against whatever now lives there. Reusing a
 * retired handle answers ERROR(BAD_HANDLE) instead.
 *
 * Only one call runs per instance at a time (a second concurrent CALL_INST
 * on the same instance answers ERROR(BUSY)); calls to *different*
 * instances do run concurrently, which is the point — an instance parked
 * on a remote-memory round trip is not holding up the others.
 *   RESULT     (n->c): [nresults u8][value u32 LE × nresults]
 *   ERROR      (n->c): [error code u8 (enum wasp_err)][utf8 detail, optional]
 *   PING       (c->n): arbitrary; PONG echoes it back
 *   IDENTIFY   (c->n): [duration_s u8, optional (default 10)]; strobes the
 *                      status LED so a human can find the board; ack RESULT
 *   REBOOT     (c->n): [mode u8, optional: 0=reboot (default),
 *                      1=USB bootloader (RP2040 only, for reflashing)];
 *                      acks RESULT, then reboots ~500 ms later
 *
 * Discovery: nodes broadcast an ANNOUNCE datagram to UDP port
 * CONFIG_WASP_ANNOUNCE_PORT every CONFIG_WASP_ANNOUNCE_INTERVAL seconds
 * (not wire-framed — it IS the payload, source IP identifies the node):
 *
 *   ['W']['A'][ANNOUNCE u8][version u8][features u8][tcp port u16 LE]
 *   [busy u8: coordinator currently connected][board_len u8][board name]
 *   [slots_total u8][slots_free u8][pool_free u32 LE]
 *
 * The three capacity fields come after the board name precisely so that
 * they are optional: a reader that stops at the end of the name sees the
 * v1 datagram, which is how existing clients keep working. They matter
 * because `busy` is now too coarse — a node with three slots and one in
 * use is not busy — and because the WAMR pool is sized at boot from
 * whatever RAM the board has left over, so capacity is a property of the
 * node rather than of the firmware. A coordinator can see that one node
 * has room for three instances and another for one without connecting to
 * either.
 *
 * Remote memory (feature bit WASP_FEAT_REMOTE_MEM): the 0x4x range is
 * node-initiated — sent by the node mid-CALL, answered by the
 * coordinator. These use the node's own seq space, so both directions
 * can be in flight on the one connection without ambiguity. A remote
 * reference packs (region_id:8 | offset:24) into a u32; the coordinator
 * is the authoritative bounds/lock checker. Failures are answered with
 * ERROR (c->n), echoing the node's seq.
 *
 *   MEM_READ   (n->c): [ref u32 LE][len u32 LE]
 *   MEM_DATA   (c->n): [data × len]
 *   MEM_WRITE  (n->c): [ref u32 LE][data]
 *   MEM_ACK    (c->n): empty (also acks UNLOCK)
 *   LOCK       (n->c): [region_id u8]; held-by-other -> ERROR(LOCKED),
 *                      or the coordinator may park the request and grant
 *                      when the region frees (queued locks are
 *                      coordinator policy — the node blocks on the
 *                      response either way, so it needs no opinion)
 *   LOCK_GRANT (c->n): [lease_ms u32 LE]
 *   UNLOCK     (n->c): [region_id u8]; ack is MEM_ACK
 *   REGION_INFO(n->c): [region_id u8]
 *   REGION_DESC(c->n): [length u32 LE]
 *
 * Atomic primitives (feature bit WASP_FEAT_ATOMICS): the coordinator
 * already serializes each request, so performing the whole
 * read-modify-write inside that window makes it race-free with no lock
 * and no lease — one round-trip replaces LOCK/READ/WRITE/UNLOCK. Both
 * respect a lock held by another node (ERROR(LOCKED)) and answer with
 * the OLD value:
 *
 *   MEM_ADD    (n->c): [ref u32 LE][delta i32 LE]; *ref += delta
 *   MEM_CAS    (n->c): [ref u32 LE][expected u32 LE][desired u32 LE];
 *                      writes desired only if *ref == expected — the
 *                      answered old value tells the node whether it won
 *   (both answered with MEM_DATA [old value u32 LE])
 */
#ifndef WASP_PROTOCOL_H_
#define WASP_PROTOCOL_H_

#define WASP_PROTO_MAGIC0 0x57 /* 'W' */
#define WASP_PROTO_MAGIC1 0x41 /* 'A' */
#define WASP_PROTO_VERSION 1

#define WASP_FRAME_HDR_SIZE 8
#define WASP_FRAME_OFF_MAGIC 0
#define WASP_FRAME_OFF_TYPE 2
#define WASP_FRAME_OFF_SEQ 3
#define WASP_FRAME_OFF_LEN 4

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
	WASP_MSG_IDENTIFY = 0x0A, /* strobe the status LED to locate the board */
	WASP_MSG_REBOOT = 0x0B,   /* reboot the node (optionally into its bootloader) */
	WASP_MSG_ANNOUNCE = 0x0C, /* UDP broadcast datagram, never on the TCP stream */

	/* Multi-instance API (WASP_FEAT_MULTI_INSTANCE). */
	WASP_MSG_MODULE_UPLOAD = 0x0D,
	WASP_MSG_INSTANTIATE = 0x0E,
	WASP_MSG_CALL_INST = 0x0F,
	WASP_MSG_DROP = 0x10,
	WASP_MSG_INVENTORY = 0x11,
	WASP_MSG_STATS = 0x12,

	/* 0x4x: node-initiated remote-memory traffic (node's seq space). */
	WASP_MSG_MEM_READ = 0x40,
	WASP_MSG_MEM_DATA = 0x41,
	WASP_MSG_MEM_WRITE = 0x42,
	WASP_MSG_MEM_ACK = 0x43,
	WASP_MSG_LOCK = 0x44,
	WASP_MSG_LOCK_GRANT = 0x45,
	WASP_MSG_UNLOCK = 0x46,
	WASP_MSG_REGION_INFO = 0x47,
	WASP_MSG_REGION_DESC = 0x48,
	WASP_MSG_MEM_ADD = 0x49,
	WASP_MSG_MEM_CAS = 0x4A,
};

/* HELLO_ACK feature bits. */
#define WASP_FEAT_REMOTE_MEM 0x01
#define WASP_FEAT_ATOMICS 0x02
#define WASP_FEAT_MULTI_INSTANCE 0x04

/* DROP kinds. */
#define WASP_DROP_INSTANCE 0
#define WASP_DROP_MODULE 1

/*
 * Module and instance handles: (gen:4 | idx:4). Four bits of index caps
 * the tables at 16, which is far above what a 64 KiB linear memory per
 * instance allows on any board this runs on, and leaves four bits of
 * generation to retire handles with.
 */
#define WASP_HANDLE_SLOTS 16
#define WASP_HANDLE(idx, gen) ((uint8_t)((((gen) & 0x0F) << 4) | ((idx) & 0x0F)))
#define WASP_HANDLE_IDX(h) ((uint8_t)((h) & 0x0F))
#define WASP_HANDLE_GEN(h) ((uint8_t)(((h) >> 4) & 0x0F))

/* Remote reference packing: (region_id:8 | offset:24). */
#define WASP_REF_REGION(ref) ((uint32_t)(ref) >> 24)
#define WASP_REF_OFFSET(ref) ((uint32_t)(ref) & 0x00FFFFFF)
#define WASP_REF_MAX_OFFSET  0x01000000

enum wasp_err {
	WASP_ERR_UNSUPPORTED = 0x01, /* valid type the node can't handle (yet) */
	WASP_ERR_BAD_TYPE = 0x02,
	WASP_ERR_TOO_LARGE = 0x03,
	WASP_ERR_NO_MEM = 0x04,
	WASP_ERR_BUSY = 0x05,
	WASP_ERR_NO_MODULE = 0x06,  /* CALL/UNLOAD with no module loaded */
	WASP_ERR_INTERNAL = 0x07,
	WASP_ERR_LOAD_FAILED = 0x08, /* module rejected by WAMR loader/instantiation */
	WASP_ERR_TRAP = 0x09,        /* module trapped during execution */
	WASP_ERR_BAD_ARGS = 0x0A,    /* malformed CALL payload or signature mismatch */
	WASP_ERR_NO_FUNC = 0x0B,     /* export not found in loaded module */
	WASP_ERR_LOCKED = 0x0C,      /* region lock held by someone else */
	WASP_ERR_NO_REGION = 0x0D,   /* remote reference names no exported region */
	WASP_ERR_BAD_HANDLE = 0x0E,  /* module/instance handle unknown or retired */
	WASP_ERR_NO_SLOTS = 0x0F,    /* table full, or the pool cannot fit another */
};

/*
 * Return codes of the wasp.* host functions as seen from inside a
 * module. Part of the module ABI — keep in sync with
 * tools/include/wasp/remote.h. Protocol failures (timeout, dropped
 * connection) are not represented here: they raise a WASM exception
 * and surface to the coordinator as ERROR(TRAP).
 */
#define WASP_REMOTE_OK 0
#define WASP_REMOTE_ELOCKED (-1)   /* region locked by someone else */
#define WASP_REMOTE_EBOUNDS (-2)   /* access outside the region */
#define WASP_REMOTE_ENOREGION (-3) /* no such exported region */
#define WASP_REMOTE_EIO (-4)       /* coordinator refused for another reason */

/* v1 calling convention limits (i32 cells only). */
#define WASP_CALL_MAX_ARGS 8
#define WASP_CALL_MAX_RESULTS 4
#define WASP_CALL_MAX_NAME_LEN 63

#endif /* WASP_PROTOCOL_H_ */
