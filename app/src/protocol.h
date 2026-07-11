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
 *   HELLO_ACK  (n->c): [proto_version u8][max_payload u32 LE]
 *   LOAD_MODULE(c->n): [.wasm binary]; success ack is an empty RESULT
 *   UNLOAD_MODULE (c->n): empty; success ack is an empty RESULT
 *   CALL       (c->n): [name_len u8][name][argc u8][arg u32 LE × argc]
 *                      v1 calling convention: i32 args/results only
 *   RESULT     (n->c): [nresults u8][value u32 LE × nresults]
 *   ERROR      (n->c): [error code u8 (enum wasp_err)][utf8 detail, optional]
 *   PING       (c->n): arbitrary; PONG echoes it back
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
};

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
};

/* v1 calling convention limits (i32 cells only). */
#define WASP_CALL_MAX_ARGS 8
#define WASP_CALL_MAX_RESULTS 4
#define WASP_CALL_MAX_NAME_LEN 63

#endif /* WASP_PROTOCOL_H_ */
