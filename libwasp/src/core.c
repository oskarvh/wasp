/*
 * libwasp core: connection lifecycle, frame I/O, the blocking request
 * loop (which services node-initiated remote-memory RPCs inline), and
 * the typed wrappers on top of it. OS-free: everything below reaches
 * the system only through port.h, so this file compiles unchanged
 * into a Linux coordinator and into Zephyr node firmware.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

const char *wasp_strerror(int err)
{
	static const char *const proto[] = {
		[WASP_ERR_UNSUPPORTED] = "UNSUPPORTED",
		[WASP_ERR_BAD_TYPE] = "BAD_TYPE",
		[WASP_ERR_TOO_LARGE] = "TOO_LARGE",
		[WASP_ERR_NO_MEM] = "NO_MEM",
		[WASP_ERR_BUSY] = "BUSY",
		[WASP_ERR_NO_MODULE] = "NO_MODULE",
		[WASP_ERR_INTERNAL] = "INTERNAL",
		[WASP_ERR_LOAD_FAILED] = "LOAD_FAILED",
		[WASP_ERR_TRAP] = "TRAP",
		[WASP_ERR_BAD_ARGS] = "BAD_ARGS",
		[WASP_ERR_NO_FUNC] = "NO_FUNC",
		[WASP_ERR_LOCKED] = "LOCKED",
		[WASP_ERR_NO_REGION] = "NO_REGION",
	};

	if (err == WASP_OK)
		return "OK";
	if (err <= -(int)WASP_ERR_UNSUPPORTED &&
	    err >= -(int)WASP_ERR_NO_REGION && proto[-err])
		return proto[-err];
	switch (err) {
	case WASP_E_TIMEOUT:
		return "timeout waiting for node";
	case WASP_E_CONN:
		return "connection failed or closed";
	case WASP_E_STREAM:
		return "protocol stream violation";
	case WASP_E_ARG:
		return "invalid argument";
	case WASP_E_TOOBIG:
		return "payload too large";
	case WASP_E_SYS:
		return "system error";
	case WASP_E_UNEXPECTED:
		return "unexpected response type";
	default:
		return "unknown error";
	}
}

/* ---- frame I/O ----------------------------------------------------- */

int wasp_send_frame(int fd, uint8_t type, uint8_t seq, const void *payload,
		    uint32_t len)
{
	uint8_t hdr[WASP_FRAME_HDR_SIZE] = {
		WASP_PROTO_MAGIC0, WASP_PROTO_MAGIC1, type, seq,
		(uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16),
		(uint8_t)(len >> 24),
	};
	int rc;

	/* Control-sized frames go out in one segment; large payloads
	 * follow separately to avoid the copy (TCP_NODELAY is set on
	 * both ends, so two segments cost no Nagle stall). */
	if (len > 0 && len <= 128) {
		uint8_t pkt[WASP_FRAME_HDR_SIZE + 128];

		memcpy(pkt, hdr, sizeof(hdr));
		memcpy(pkt + sizeof(hdr), payload, len);
		return wasp_net_send(fd, pkt, sizeof(hdr) + len);
	}
	rc = wasp_net_send(fd, hdr, sizeof(hdr));
	if (rc == WASP_OK && len > 0)
		rc = wasp_net_send(fd, payload, len);
	return rc;
}

/* Receive one frame into n->rx; outputs type/seq/len. */
static int recv_frame(wasp_node *n, uint8_t *type, uint8_t *seq,
		      uint32_t *len)
{
	uint8_t hdr[WASP_FRAME_HDR_SIZE];
	uint32_t plen;
	int rc = wasp_net_recv_exact(n->fd, hdr, sizeof(hdr));

	if (rc != WASP_OK)
		return rc;
	if (hdr[0] != WASP_PROTO_MAGIC0 || hdr[1] != WASP_PROTO_MAGIC1)
		return WASP_E_STREAM;
	plen = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
	       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
	if (plen > n->rx_cap)
		return WASP_E_STREAM; /* can't resync mid-payload */
	rc = wasp_net_recv_exact(n->fd, n->rx, plen);
	if (rc != WASP_OK)
		return rc;
	*type = hdr[WASP_FRAME_OFF_TYPE];
	*seq = hdr[WASP_FRAME_OFF_SEQ];
	*len = plen;
	return WASP_OK;
}

/* ---- connection lifecycle ------------------------------------------ */

void wasp_node_attach(wasp_node *n, wasp_ctx *ctx, int fd, uint8_t *rx,
		      uint32_t rx_cap, uint32_t timeout_ms)
{
	memset(n, 0, sizeof(*n));
	n->ctx = ctx;
	n->fd = fd;
	n->rx = rx;
	n->rx_cap = rx_cap;
	n->timeout_ms = timeout_ms;
	strncpy(n->owner, "node", sizeof(n->owner) - 1);
	/* Random-ish start so a stale response from a previous
	 * connection can't accidentally match our first request. */
	n->seq = (uint8_t)(wasp_now_ms() ^ (uintptr_t)n ^ (unsigned)fd);
}

int wasp_node_connect_static(wasp_node *n, wasp_ctx *ctx, uint8_t *rx,
			     uint32_t rx_cap, const char *host, uint16_t port,
			     uint32_t timeout_ms)
{
	int fd;

	if (!n || !ctx || !rx || rx_cap < 64 || !host || timeout_ms == 0)
		return WASP_E_ARG;
	fd = wasp_net_connect(host, port, timeout_ms);
	if (fd < 0)
		return WASP_E_CONN;
	wasp_node_attach(n, ctx, fd, rx, rx_cap, timeout_ms);
	return WASP_OK;
}

wasp_node *wasp_connect(wasp_ctx *ctx, const char *host, uint16_t port,
			uint32_t timeout_ms)
{
	wasp_node *n;
	uint8_t *rx;

	if (!ctx || !host || timeout_ms == 0)
		return NULL;
	n = malloc(sizeof(*n));
	rx = malloc(WASP_RX_CAP_DEFAULT);
	if (!n || !rx) {
		free(n);
		free(rx);
		return NULL;
	}
	if (wasp_node_connect_static(n, ctx, rx, WASP_RX_CAP_DEFAULT, host,
				     port, timeout_ms) != WASP_OK) {
		free(n);
		free(rx);
		return NULL;
	}
	n->heap = 1;
	return n;
}

void wasp_node_kill(wasp_node *n)
{
	if (n && n->fd >= 0)
		wasp_net_shutdown(n->fd);
}

void wasp_node_free(wasp_node *n)
{
	if (!n)
		return;
	if (n->fd >= 0)
		wasp_net_close(n->fd);
	n->fd = -1;
	if (n->heap) {
		free(n->rx);
		free(n);
	}
}

void wasp_node_set_owner(wasp_node *n, const char *owner)
{
	if (!n || !owner)
		return;
	strncpy(n->owner, owner, sizeof(n->owner) - 1);
	n->owner[sizeof(n->owner) - 1] = '\0';
}

void wasp_node_set_queue_locks(wasp_node *n, int on)
{
	if (n)
		n->queue_locks = on;
}

void wasp_node_set_tag(wasp_node *n, uint32_t tag)
{
	if (n)
		n->tag = tag;
}

const char *wasp_node_error_detail(wasp_node *n)
{
	return n ? n->detail : "";
}

/* ---- the request loop ---------------------------------------------- */

static int is_node_request(uint8_t type)
{
	switch (type) {
	case WASP_MSG_MEM_READ:
	case WASP_MSG_MEM_WRITE:
	case WASP_MSG_LOCK:
	case WASP_MSG_UNLOCK:
	case WASP_MSG_REGION_INFO:
	case WASP_MSG_MEM_ADD:
	case WASP_MSG_MEM_CAS:
		return 1;
	default:
		return 0;
	}
}

int wasp_request(wasp_node *n, uint8_t type, const void *payload,
		 uint32_t len, uint8_t *rtype, uint8_t *rseq,
		 const uint8_t **rpayload, uint32_t *rlen)
{
	uint8_t seq;
	int rc;

	if (!n || (len > 0 && !payload) || !rtype || !rpayload || !rlen)
		return WASP_E_ARG;
	seq = n->seq = (uint8_t)(n->seq + 1);
	rc = wasp_send_frame(n->fd, type, seq, payload, len);
	if (rc != WASP_OK)
		return rc;

	for (;;) {
		uint8_t t, s;
		uint32_t l;

		rc = recv_frame(n, &t, &s, &l);
		if (rc != WASP_OK)
			return rc;
		if (is_node_request(t)) {
			/* Issued by the node mid-CALL; serve inline. */
			rc = wasp_host_serve(n, t, s, n->rx, l);
			if (rc != WASP_OK)
				return rc;
			continue;
		}
		if (s != seq)
			return WASP_E_STREAM;
		*rtype = t;
		if (rseq)
			*rseq = s;
		*rpayload = n->rx;
		*rlen = l;
		return WASP_OK;
	}
}

/* ---- typed wrappers ------------------------------------------------ */

/* Interpret an ERROR response: stash the detail, return -code. */
static int node_error(wasp_node *n, const uint8_t *p, uint32_t len)
{
	uint32_t dlen;

	if (len == 0)
		return WASP_E_UNEXPECTED;
	dlen = len - 1;
	if (dlen >= WASP_DETAIL_MAX)
		dlen = WASP_DETAIL_MAX - 1;
	memcpy(n->detail, p + 1, dlen);
	n->detail[dlen] = '\0';
	return -(int)p[0];
}

int wasp_hello(wasp_node *n, uint8_t *version, uint32_t *max_payload,
	       uint8_t *features)
{
	uint8_t ver = WASP_PROTO_VERSION;
	const uint8_t *p;
	uint8_t t;
	uint32_t l;
	int rc;

	if (!n)
		return WASP_E_ARG;
	rc = wasp_request(n, WASP_MSG_HELLO, &ver, 1, &t, NULL, &p, &l);
	if (rc != WASP_OK)
		return rc;
	if (t == WASP_MSG_ERROR)
		return node_error(n, p, l);
	if (t != WASP_MSG_HELLO_ACK || l < 5)
		return WASP_E_UNEXPECTED;
	n->max_payload = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
			 ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
	if (version)
		*version = p[0];
	if (max_payload)
		*max_payload = n->max_payload;
	if (features)
		*features = l >= 6 ? p[5] : 0;
	return WASP_OK;
}

static int expect_empty_result(wasp_node *n, uint8_t req,
			       const void *payload, uint32_t len)
{
	const uint8_t *p;
	uint8_t t;
	uint32_t l;
	int rc = wasp_request(n, req, payload, len, &t, NULL, &p, &l);

	if (rc != WASP_OK)
		return rc;
	if (t == WASP_MSG_ERROR)
		return node_error(n, p, l);
	return t == WASP_MSG_RESULT ? WASP_OK : WASP_E_UNEXPECTED;
}

int wasp_load(wasp_node *n, const void *wasm, uint32_t len)
{
	if (!n || !wasm || len == 0)
		return WASP_E_ARG;
	if (n->max_payload && len > n->max_payload)
		return WASP_E_TOOBIG;
	return expect_empty_result(n, WASP_MSG_LOAD_MODULE, wasm, len);
}

int wasp_unload(wasp_node *n)
{
	if (!n)
		return WASP_E_ARG;
	return expect_empty_result(n, WASP_MSG_UNLOAD_MODULE, NULL, 0);
}

int wasp_call(wasp_node *n, const char *fn, const int32_t *argv,
	      uint32_t argc, int32_t results[WASP_CALL_MAX_RESULTS])
{
	uint8_t req[2 + WASP_CALL_MAX_NAME_LEN + 4 * WASP_CALL_MAX_ARGS];
	size_t name_len, off;
	const uint8_t *p;
	uint8_t t;
	uint32_t l, i, nres;
	int rc;

	if (!n || !fn || (argc > 0 && !argv) || argc > WASP_CALL_MAX_ARGS)
		return WASP_E_ARG;
	name_len = strlen(fn);
	if (name_len == 0 || name_len > WASP_CALL_MAX_NAME_LEN)
		return WASP_E_ARG;

	req[0] = (uint8_t)name_len;
	memcpy(req + 1, fn, name_len);
	off = 1 + name_len;
	req[off++] = (uint8_t)argc;
	for (i = 0; i < argc; i++) {
		uint32_t v = (uint32_t)argv[i];

		req[off++] = (uint8_t)v;
		req[off++] = (uint8_t)(v >> 8);
		req[off++] = (uint8_t)(v >> 16);
		req[off++] = (uint8_t)(v >> 24);
	}

	rc = wasp_request(n, WASP_MSG_CALL, req, (uint32_t)off, &t, NULL, &p,
			  &l);
	if (rc != WASP_OK)
		return rc;
	if (t == WASP_MSG_ERROR)
		return node_error(n, p, l);
	if (t != WASP_MSG_RESULT || l < 1)
		return WASP_E_UNEXPECTED;
	nres = p[0];
	if (nres > WASP_CALL_MAX_RESULTS || l < 1 + 4 * nres)
		return WASP_E_UNEXPECTED;
	for (i = 0; i < nres && results; i++) {
		uint32_t v = (uint32_t)p[1 + 4 * i] |
			     ((uint32_t)p[2 + 4 * i] << 8) |
			     ((uint32_t)p[3 + 4 * i] << 16) |
			     ((uint32_t)p[4 + 4 * i] << 24);

		results[i] = (int32_t)v;
	}
	return (int)nres;
}
