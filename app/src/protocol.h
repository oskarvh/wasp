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
 *   LOAD_MODULE(c->n): [.wasm binary]
 *   CALL       (c->n): TBD with the WAMR lifecycle work
 *   RESULT     (n->c): TBD with the WAMR lifecycle work
 *   ERROR      (n->c): [error code u8 (enum wasp_err)]
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
	WASP_ERR_NO_MODULE = 0x06,
	WASP_ERR_INTERNAL = 0x07,
};

#endif /* WASP_PROTOCOL_H_ */
