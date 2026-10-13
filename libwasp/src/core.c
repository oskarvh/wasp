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
		[WASP_ERR_BAD_HANDLE] = "BAD_HANDLE",
		[WASP_ERR_NO_SLOTS] = "NO_SLOTS",
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
	case WASP_E_AGAIN:
		return "async call still running";
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

/* ---- async call slots ---------------------------------------------- */

static int parse_call_result(wasp_node *n, uint8_t t, const uint8_t *p,
			     uint32_t l,
			     int32_t results[WASP_CALL_MAX_RESULTS]);

static struct wasp_call_slot *slot_by_seq(wasp_node *n, uint8_t seq)
{
	int i;

	for (i = 0; i < WASP_MAX_INFLIGHT; i++)
		if (n->calls[i].in_use && !n->calls[i].done &&
		    n->calls[i].seq == seq)
			return &n->calls[i];
	return NULL;
}

static void complete_slot(wasp_node *n, struct wasp_call_slot *sl, uint8_t t,
			  const uint8_t *p, uint32_t l)
{
	sl->rc = parse_call_result(n, t, p, l, sl->results);
	sl->done = 1;
}

static struct wasp_call_slot *slot_alloc(wasp_node *n)
{
	int i;

	for (i = 0; i < WASP_MAX_INFLIGHT; i++)
		if (!n->calls[i].in_use)
			return &n->calls[i];
	return NULL;
}

/* A transport failure invalidates every outstanding call on the node. */
static void slots_abort(wasp_node *n)
{
	int i;

	for (i = 0; i < WASP_MAX_INFLIGHT; i++) {
		n->calls[i].in_use = 0;
		n->calls[i].done = 0;
	}
}

int wasp_call_inflight(wasp_node *n)
{
	int i, count = 0;

	if (!n)
		return WASP_E_ARG;
	for (i = 0; i < WASP_MAX_INFLIGHT; i++)
		if (n->calls[i].in_use)
			count++;
	return count;
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
		if (s != seq) {
			/* An async call's response, arriving while we waited
			 * on a synchronous one. Parse it into its slot now:
			 * n->rx holds a single frame, so leaving it for the
			 * poller would lose it to the next frame.
			 */
			struct wasp_call_slot *sl = slot_by_seq(n, s);

			if (sl) {
				complete_slot(n, sl, t, n->rx, l);
				continue;
			}
			return WASP_E_STREAM;
		}
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

/* Encode the CALL request payload; returns its length or WASP_E_ARG. */
static int build_call_req(uint8_t *req, const char *fn, const int32_t *argv,
			  uint32_t argc)
{
	size_t name_len, off;
	uint32_t i;

	if (!fn || (argc > 0 && !argv) || argc > WASP_CALL_MAX_ARGS)
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
	return (int)off;
}

/* Interpret a CALL response frame with wasp_call's return convention. */
static int parse_call_result(wasp_node *n, uint8_t t, const uint8_t *p,
			     uint32_t l,
			     int32_t results[WASP_CALL_MAX_RESULTS])
{
	uint32_t i, nres;

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

int wasp_call(wasp_node *n, const char *fn, const int32_t *argv,
	      uint32_t argc, int32_t results[WASP_CALL_MAX_RESULTS])
{
	uint8_t req[2 + WASP_CALL_MAX_NAME_LEN + 4 * WASP_CALL_MAX_ARGS];
	const uint8_t *p;
	uint8_t t;
	uint32_t l;
	int len, rc;

	if (!n)
		return WASP_E_ARG;
	len = build_call_req(req, fn, argv, argc);
	if (len < 0)
		return len;
	rc = wasp_request(n, WASP_MSG_CALL, req, (uint32_t)len, &t, NULL, &p,
			  &l);
	if (rc != WASP_OK)
		return rc;
	return parse_call_result(n, t, p, l, results);
}

/* ---- several instances per node ------------------------------------- */

/*
 * MODULE_UPLOAD [n_inst][.wasm] -> RESULT [mod][n][inst x n].
 *
 * The payload is the count followed by the binary, so it cannot be sent
 * straight from the caller's buffer without a copy. Rather than demand a
 * scratch buffer of the caller or allocate one, write the count and the
 * binary as one frame built in n->rx, which is already sized for a whole
 * module (it has to hold any request or response).
 */
int wasp_upload(wasp_node *n, const void *wasm, uint32_t len, uint8_t n_inst,
		uint8_t *mod_out, uint8_t *inst_out, uint8_t inst_cap)
{
	const uint8_t *p;
	uint8_t t;
	uint32_t l;
	int rc, i, count;

	if (!n || !wasm || len == 0)
		return WASP_E_ARG;
	if (n->max_payload && len + 1 > n->max_payload)
		return WASP_E_TOOBIG;
	if (len + 1 > n->rx_cap)
		return WASP_E_TOOBIG;

	n->rx[0] = n_inst;
	memcpy(n->rx + 1, wasm, len);
	rc = wasp_request(n, WASP_MSG_MODULE_UPLOAD, n->rx, len + 1, &t, NULL,
			  &p, &l);
	if (rc != WASP_OK)
		return rc;
	if (t == WASP_MSG_ERROR)
		return node_error(n, p, l);
	if (t != WASP_MSG_RESULT || l < 2)
		return WASP_E_UNEXPECTED;

	count = p[1];
	if (l < (uint32_t)(2 + count))
		return WASP_E_UNEXPECTED;
	if (mod_out)
		*mod_out = p[0];
	for (i = 0; i < count && inst_out && i < inst_cap; i++)
		inst_out[i] = p[2 + i];
	return count;
}

int wasp_instantiate(wasp_node *n, uint8_t mod, uint8_t count,
		     uint8_t *inst_out, uint8_t inst_cap)
{
	uint8_t req[2];
	const uint8_t *p;
	uint8_t t;
	uint32_t l;
	int rc, i, made;

	if (!n || count == 0)
		return WASP_E_ARG;
	req[0] = mod;
	req[1] = count;
	rc = wasp_request(n, WASP_MSG_INSTANTIATE, req, sizeof(req), &t, NULL,
			  &p, &l);
	if (rc != WASP_OK)
		return rc;
	if (t == WASP_MSG_ERROR)
		return node_error(n, p, l);
	if (t != WASP_MSG_RESULT || l < 1)
		return WASP_E_UNEXPECTED;

	made = p[0];
	if (l < (uint32_t)(1 + made))
		return WASP_E_UNEXPECTED;
	for (i = 0; i < made && inst_out && i < inst_cap; i++)
		inst_out[i] = p[1 + i];
	return made;
}

int wasp_call_inst(wasp_node *n, uint8_t inst, const char *fn,
		   const int32_t *argv, uint32_t argc,
		   int32_t results[WASP_CALL_MAX_RESULTS])
{
	uint8_t req[3 + WASP_CALL_MAX_NAME_LEN + 4 * WASP_CALL_MAX_ARGS];
	const uint8_t *p;
	uint8_t t;
	uint32_t l;
	int len, rc;

	if (!n)
		return WASP_E_ARG;
	req[0] = inst;
	len = build_call_req(req + 1, fn, argv, argc);
	if (len < 0)
		return len;
	rc = wasp_request(n, WASP_MSG_CALL_INST, req, (uint32_t)len + 1, &t,
			  NULL, &p, &l);
	if (rc != WASP_OK)
		return rc;
	return parse_call_result(n, t, p, l, results);
}

static int drop(wasp_node *n, uint8_t kind, uint8_t handle)
{
	uint8_t req[2];

	if (!n)
		return WASP_E_ARG;
	req[0] = kind;
	req[1] = handle;
	return expect_empty_result(n, WASP_MSG_DROP, req, sizeof(req));
}

int wasp_drop_instance(wasp_node *n, uint8_t inst)
{
	return drop(n, WASP_DROP_INSTANCE, inst);
}

int wasp_drop_module(wasp_node *n, uint8_t mod)
{
	return drop(n, WASP_DROP_MODULE, mod);
}

int wasp_inventory_get(wasp_node *n, wasp_inventory *out)
{
	const uint8_t *p;
	uint8_t t;
	uint32_t l, at;
	int rc, i, j;

	if (!n || !out)
		return WASP_E_ARG;
	rc = wasp_request(n, WASP_MSG_INVENTORY, NULL, 0, &t, NULL, &p, &l);
	if (rc != WASP_OK)
		return rc;
	if (t == WASP_MSG_ERROR)
		return node_error(n, p, l);
	if (t != WASP_MSG_RESULT || l < 7)
		return WASP_E_UNEXPECTED;

	memset(out, 0, sizeof(*out));
	out->slots_total = p[0];
	out->slots_free = p[1];
	out->pool_free = (uint32_t)p[2] | ((uint32_t)p[3] << 8) |
			 ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
	out->n_modules = p[6];
	if (out->n_modules > WASP_HANDLE_SLOTS)
		return WASP_E_UNEXPECTED;

	at = 7;
	for (i = 0; i < out->n_modules; i++) {
		uint8_t n_inst;

		if (at + 2 > l)
			return WASP_E_UNEXPECTED;
		out->modules[i].mod = p[at];
		n_inst = p[at + 1];
		if (n_inst > WASP_HANDLE_SLOTS || at + 2 + n_inst > l)
			return WASP_E_UNEXPECTED;
		out->modules[i].n_inst = n_inst;
		for (j = 0; j < n_inst; j++)
			out->modules[i].inst[j] = p[at + 2 + j];
		at += 2 + n_inst;
	}
	return WASP_OK;
}

/* ---- async calls --------------------------------------------------- */

static int call_async(wasp_node *n, int has_inst, uint8_t inst, const char *fn,
		      const int32_t *argv, uint32_t argc)
{
	uint8_t req[3 + WASP_CALL_MAX_NAME_LEN + 4 * WASP_CALL_MAX_ARGS];
	struct wasp_call_slot *sl;
	uint32_t off = has_inst ? 1 : 0;
	int len, rc;

	if (!n)
		return WASP_E_ARG;
	sl = slot_alloc(n);
	if (!sl)
		return WASP_E_AGAIN; /* WASP_MAX_INFLIGHT already outstanding */

	if (has_inst)
		req[0] = inst;
	len = build_call_req(req + off, fn, argv, argc);
	if (len < 0)
		return len;

	sl->seq = n->seq = (uint8_t)(n->seq + 1);
	rc = wasp_send_frame(n->fd,
			     has_inst ? WASP_MSG_CALL_INST : WASP_MSG_CALL,
			     sl->seq, req, (uint32_t)len + off);
	if (rc != WASP_OK)
		return rc;
	sl->in_use = 1;
	sl->done = 0;
	sl->inst = inst;
	sl->has_inst = has_inst;
	return WASP_OK;
}

int wasp_call_async(wasp_node *n, const char *fn, const int32_t *argv,
		    uint32_t argc)
{
	return call_async(n, 0, 0, fn, argv, argc);
}

int wasp_call_inst_async(wasp_node *n, uint8_t inst, const char *fn,
			 const int32_t *argv, uint32_t argc)
{
	return call_async(n, 1, inst, fn, argv, argc);
}

/* Hand back the first slot whose response has been parsed. */
static int harvest(wasp_node *n, uint8_t *inst_out,
		   int32_t results[WASP_CALL_MAX_RESULTS])
{
	int i, j;

	for (i = 0; i < WASP_MAX_INFLIGHT; i++) {
		struct wasp_call_slot *sl = &n->calls[i];

		if (!sl->in_use || !sl->done)
			continue;
		if (inst_out)
			*inst_out = sl->inst;
		for (j = 0; j < WASP_CALL_MAX_RESULTS && results; j++)
			results[j] = sl->results[j];
		sl->in_use = 0;
		sl->done = 0;
		return sl->rc;
	}
	return WASP_E_AGAIN;
}

int wasp_call_poll_any(wasp_node *n, uint8_t *inst_out,
		       int32_t results[WASP_CALL_MAX_RESULTS])
{
	int rc;

	if (!n)
		return WASP_E_ARG;
	if (wasp_call_inflight(n) == 0)
		return WASP_E_ARG;

	/* A response may already have been parsed by a synchronous request
	 * that ran while this call was outstanding.
	 */
	rc = harvest(n, inst_out, results);
	if (rc != WASP_E_AGAIN)
		return rc;

	while (wasp_net_readable(n->fd, 0)) {
		struct wasp_call_slot *sl;
		uint8_t t, s;
		uint32_t l;

		rc = recv_frame(n, &t, &s, &l);
		if (rc != WASP_OK) {
			slots_abort(n);
			return rc;
		}
		if (is_node_request(t)) {
			rc = wasp_host_serve(n, t, s, n->rx, l);
			if (rc != WASP_OK) {
				slots_abort(n);
				return rc;
			}
			continue;
		}
		sl = slot_by_seq(n, s);
		if (!sl) {
			slots_abort(n);
			return WASP_E_STREAM;
		}
		complete_slot(n, sl, t, n->rx, l);
		return harvest(n, inst_out, results);
	}
	return WASP_E_AGAIN;
}

int wasp_call_poll(wasp_node *n, int32_t results[WASP_CALL_MAX_RESULTS])
{
	return wasp_call_poll_any(n, NULL, results);
}

/*
 * Blocking form of wasp_call_poll_any.
 *
 * wasp_call_poll_any polls with a zero timeout, which is right for a
 * conductor interleaving several nodes from one thread: it returns
 * WASP_E_AGAIN at once and the caller moves on. It is wrong for waiting
 * on the instances of a *single* node, where the only options would be a
 * busy spin or a sleep — and a sleep long enough to be cheap is
 * comparable to a remote-memory round trip, so it would pay for the
 * latency hiding it was meant to collect.
 *
 * This waits on the socket instead, so the node's own RPCs are answered
 * the moment they arrive.
 */
int wasp_call_wait_any(wasp_node *n, uint8_t *inst_out,
		       int32_t results[WASP_CALL_MAX_RESULTS],
		       uint32_t timeout_ms)
{
	uint64_t deadline;
	int rc;

	if (!n)
		return WASP_E_ARG;
	if (wasp_call_inflight(n) == 0)
		return WASP_E_ARG;

	/* A response may already be sitting parsed in its slot. */
	rc = harvest(n, inst_out, results);
	if (rc != WASP_E_AGAIN)
		return rc;

	deadline = wasp_now_ms() + timeout_ms;
	for (;;) {
		struct wasp_call_slot *sl;
		uint64_t now = wasp_now_ms();
		uint32_t left = (now >= deadline) ? 0 :
				(uint32_t)(deadline - now);
		uint8_t t, s;
		uint32_t l;

		if (!wasp_net_readable(n->fd, left))
			return WASP_E_TIMEOUT;

		rc = recv_frame(n, &t, &s, &l);
		if (rc != WASP_OK) {
			slots_abort(n);
			return rc;
		}
		if (is_node_request(t)) {
			rc = wasp_host_serve(n, t, s, n->rx, l);
			if (rc != WASP_OK) {
				slots_abort(n);
				return rc;
			}
			continue;
		}
		sl = slot_by_seq(n, s);
		if (!sl) {
			slots_abort(n);
			return WASP_E_STREAM;
		}
		complete_slot(n, sl, t, n->rx, l);
		return harvest(n, inst_out, results);
	}
}
