/*
 * libwasp memory host: the coordinator side of the wasp.* module API.
 * Serves node-initiated MEM/LOCK/REGION_INFO requests against the
 * ctx's registered regions, under one mutex shared by every
 * connection — that serialization is the atomicity guarantee for
 * MEM_ADD/MEM_CAS and the exactness guarantee for locks. Behavior
 * mirrors tools/wasp_client.py. OS access only via port.h.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* ---- ctx lifecycle ------------------------------------------------- */

void wasp_ctx_init(wasp_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	wasp_mutex_init(&ctx->mem_mu);
	wasp_mutex_init(&ctx->ev_mu);
	wasp_cond_init(&ctx->mem_cv);
	wasp_cond_init(&ctx->ev_cv);
}

wasp_ctx *wasp_ctx_new(void)
{
	wasp_ctx *ctx = malloc(sizeof(*ctx));

	if (!ctx)
		return NULL;
	wasp_ctx_init(ctx);
	return ctx;
}

void wasp_ctx_free(wasp_ctx *ctx)
{
	if (!ctx)
		return;
	wasp_mutex_destroy(&ctx->mem_mu);
	wasp_mutex_destroy(&ctx->ev_mu);
	wasp_cond_destroy(&ctx->mem_cv);
	wasp_cond_destroy(&ctx->ev_cv);
	free(ctx);
}

/* ---- regions ------------------------------------------------------- */

int wasp_region_register(wasp_ctx *ctx, uint8_t region_id, void *buf,
			 uint32_t len)
{
	if (!ctx || !buf || len == 0 || len > WASP_REF_MAX_OFFSET ||
	    (uint32_t)region_id >= WASP_MAX_REGIONS)
		return WASP_E_ARG;
	wasp_mutex_lock(&ctx->mem_mu);
	ctx->regions[region_id].buf = buf;
	ctx->regions[region_id].len = len;
	wasp_mutex_unlock(&ctx->mem_mu);
	return WASP_OK;
}

int wasp_region_unregister(wasp_ctx *ctx, uint8_t region_id)
{
	if (!ctx || (uint32_t)region_id >= WASP_MAX_REGIONS)
		return WASP_E_ARG;
	wasp_mutex_lock(&ctx->mem_mu);
	ctx->regions[region_id].buf = NULL;
	ctx->regions[region_id].len = 0;
	wasp_mutex_unlock(&ctx->mem_mu);
	return WASP_OK;
}

/* Registered region, or NULL (also for out-of-table ids). Call with
 * mem_mu held. */
static struct wasp_region *region_get(wasp_ctx *ctx, uint32_t region)
{
	if (region >= WASP_MAX_REGIONS || !ctx->regions[region].buf)
		return NULL;
	return &ctx->regions[region];
}

/* ---- locks --------------------------------------------------------- */

/* Current unexpired lease holder, or NULL. Expired -> revoked.
 * Call with mem_mu held; region must be < WASP_MAX_REGIONS. */
static const char *lock_holder(wasp_ctx *ctx, uint32_t region)
{
	struct wasp_lock *lk = &ctx->locks[region];

	if (!lk->held)
		return NULL;
	if (lk->expiry_ms <= wasp_now_ms()) {
		lk->held = 0;
		return NULL;
	}
	return lk->owner;
}

/* Held by nobody, or by `owner` itself. Call with mem_mu held. */
static int lock_permits(wasp_ctx *ctx, uint32_t region, const char *owner)
{
	const char *h = lock_holder(ctx, region);

	return h == NULL || strcmp(h, owner) == 0;
}

static void lock_set(wasp_ctx *ctx, uint32_t region, const char *owner,
		     uint32_t lease_ms)
{
	struct wasp_lock *lk = &ctx->locks[region];

	strncpy(lk->owner, owner, sizeof(lk->owner) - 1);
	lk->owner[sizeof(lk->owner) - 1] = '\0';
	lk->expiry_ms = wasp_now_ms() + lease_ms;
	lk->held = 1;
}

int wasp_lock_force(wasp_ctx *ctx, uint8_t region_id, const char *owner,
		    uint32_t lease_ms)
{
	if (!ctx || !owner || (uint32_t)region_id >= WASP_MAX_REGIONS)
		return WASP_E_ARG;
	wasp_mutex_lock(&ctx->mem_mu);
	lock_set(ctx, region_id, owner, lease_ms);
	wasp_mutex_unlock(&ctx->mem_mu);
	return WASP_OK;
}

int wasp_lock_clear(wasp_ctx *ctx, uint8_t region_id)
{
	if (!ctx || (uint32_t)region_id >= WASP_MAX_REGIONS)
		return WASP_E_ARG;
	wasp_mutex_lock(&ctx->mem_mu);
	ctx->locks[region_id].held = 0;
	wasp_cond_broadcast(&ctx->mem_cv);
	wasp_mutex_unlock(&ctx->mem_mu);
	return WASP_OK;
}

int wasp_lock_holder(wasp_ctx *ctx, uint8_t region_id, char *out,
		     uint32_t cap)
{
	const char *h;
	int held;

	if (!ctx || (uint32_t)region_id >= WASP_MAX_REGIONS)
		return WASP_E_ARG;
	wasp_mutex_lock(&ctx->mem_mu);
	h = lock_holder(ctx, region_id);
	held = h != NULL;
	if (h && out && cap > 0) {
		strncpy(out, h, cap - 1);
		out[cap - 1] = '\0';
	}
	wasp_mutex_unlock(&ctx->mem_mu);
	return held;
}

/* ---- events -------------------------------------------------------- */

static void ev_push(wasp_ctx *ctx, const wasp_event *ev)
{
	wasp_mutex_lock(&ctx->ev_mu);
	if (ctx->ev_count == WASP_EVQ_CAP) {
		ctx->ev_dropped++;
	} else {
		ctx->evq[(ctx->ev_head + ctx->ev_count) % WASP_EVQ_CAP] = *ev;
		ctx->ev_count++;
		wasp_cond_signal(&ctx->ev_cv);
	}
	wasp_mutex_unlock(&ctx->ev_mu);
}

int wasp_poll_event(wasp_ctx *ctx, wasp_event *out, uint32_t timeout_ms)
{
	uint64_t deadline;
	int got = 0;

	if (!ctx || !out)
		return WASP_E_ARG;
	deadline = wasp_now_ms() + timeout_ms;

	wasp_mutex_lock(&ctx->ev_mu);
	while (ctx->ev_count == 0) {
		uint64_t now = wasp_now_ms();

		if (now >= deadline)
			break;
		wasp_cond_timedwait_ms(&ctx->ev_cv, &ctx->ev_mu,
				       (uint32_t)(deadline - now));
	}
	if (ctx->ev_count > 0) {
		*out = ctx->evq[ctx->ev_head];
		ctx->ev_head = (ctx->ev_head + 1) % WASP_EVQ_CAP;
		ctx->ev_count--;
		got = 1;
	}
	wasp_mutex_unlock(&ctx->ev_mu);
	return got;
}

/* ---- serving node requests ----------------------------------------- */

/* Response under construction: type + payload (large payloads point
 * into the node's rx buffer). */
struct resp {
	uint8_t type;
	const uint8_t *payload;
	uint32_t len;
	uint8_t small[64]; /* inline storage for short payloads */
};

static void resp_error(struct resp *r, uint8_t code, const char *detail)
{
	size_t dlen = detail ? strlen(detail) : 0;

	if (dlen > sizeof(r->small) - 1)
		dlen = sizeof(r->small) - 1;
	r->type = WASP_MSG_ERROR;
	r->small[0] = code;
	if (dlen)
		memcpy(r->small + 1, detail, dlen);
	r->payload = r->small;
	r->len = (uint32_t)(1 + dlen);
}

static void resp_u32(struct resp *r, uint8_t type, uint32_t v)
{
	r->type = type;
	r->small[0] = (uint8_t)v;
	r->small[1] = (uint8_t)(v >> 8);
	r->small[2] = (uint8_t)(v >> 16);
	r->small[3] = (uint8_t)(v >> 24);
	r->payload = r->small;
	r->len = 4;
}

static void resp_empty(struct resp *r, uint8_t type)
{
	r->type = type;
	r->payload = r->small;
	r->len = 0;
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Grant the lock to n and build the LOCK_GRANT response. */
static void lock_grant(wasp_node *n, uint32_t region, struct resp *r)
{
	lock_set(n->ctx, region, n->owner, WASP_LOCK_LEASE_MS);
	resp_u32(r, WASP_MSG_LOCK_GRANT, WASP_LOCK_LEASE_MS);
}

/*
 * Grant, or (queued mode) park FIFO-fair until the holder frees the
 * region. Runs with mem_mu held; the cond wait releases it so the
 * holder's own UNLOCK can be serviced by its thread meanwhile.
 */
static void lock_response(wasp_node *n, uint32_t region, struct resp *r)
{
	wasp_ctx *ctx = n->ctx;
	struct wasp_waiter w = { .owner = n->owner };
	struct wasp_waiter **pp;
	uint64_t deadline;

	if (lock_permits(ctx, region, n->owner)) {
		lock_grant(n, region, r);
		return;
	}
	if (!n->queue_locks) {
		resp_error(r, WASP_ERR_LOCKED, NULL);
		return;
	}

	if (ctx->qtail[region])
		ctx->qtail[region]->next = &w;
	else
		ctx->qhead[region] = &w;
	ctx->qtail[region] = &w;
	deadline = wasp_now_ms() + WASP_LOCK_QUEUE_WAIT_MS;

	for (;;) {
		if (strcmp(ctx->qhead[region]->owner, n->owner) == 0 &&
		    lock_permits(ctx, region, n->owner)) {
			lock_grant(n, region, r);
			break;
		}
		if (wasp_now_ms() >= deadline) {
			/* Queue too deep for the node's RPC timeout:
			 * fail fast after all, don't trap the module. */
			resp_error(r, WASP_ERR_LOCKED, NULL);
			break;
		}
		/* Short timeout so lazy lease expiry is noticed too. */
		wasp_cond_timedwait_ms(&ctx->mem_cv, &ctx->mem_mu, 50);
	}

	for (pp = &ctx->qhead[region]; *pp; pp = &(*pp)->next) {
		if (*pp == &w) {
			*pp = w.next;
			break;
		}
	}
	if (ctx->qtail[region] == &w) {
		struct wasp_waiter *last = ctx->qhead[region];

		while (last && last->next)
			last = last->next;
		ctx->qtail[region] = last;
	}
}

/* Build the response to one node request. Call with mem_mu held. */
static void node_response(wasp_node *n, uint8_t rtype, uint8_t *p,
			  uint32_t len, struct resp *r)
{
	wasp_ctx *ctx = n->ctx;
	struct wasp_region *reg;
	uint32_t ref, region, offset;

	if (rtype == WASP_MSG_LOCK || rtype == WASP_MSG_UNLOCK ||
	    rtype == WASP_MSG_REGION_INFO) {
		if (len != 1) {
			resp_error(r, WASP_ERR_BAD_ARGS, "malformed request");
			return;
		}
		region = p[0];
		reg = region_get(ctx, region);
		if (!reg) {
			resp_error(r, WASP_ERR_NO_REGION, NULL);
			return;
		}
		if (rtype == WASP_MSG_REGION_INFO) {
			resp_u32(r, WASP_MSG_REGION_DESC, reg->len);
			return;
		}
		if (rtype == WASP_MSG_LOCK) {
			lock_response(n, region, r);
			return;
		}
		/* UNLOCK */
		const char *h = lock_holder(ctx, region);

		if (!h || strcmp(h, n->owner) != 0) {
			resp_error(r, WASP_ERR_BAD_ARGS, "not lock holder");
			return;
		}
		ctx->locks[region].held = 0;
		wasp_cond_broadcast(&ctx->mem_cv); /* wake queued LOCKs */
		resp_empty(r, WASP_MSG_MEM_ACK);
		return;
	}

	/*
	 * MEM_ADD [ref][delta i32] / MEM_CAS [ref][expected][desired]:
	 * the whole read-modify-write happens inside this serialized
	 * window, which is what makes it atomic across all nodes.
	 */
	if (rtype == WASP_MSG_MEM_ADD || rtype == WASP_MSG_MEM_CAS) {
		uint32_t want = rtype == WASP_MSG_MEM_ADD ? 8 : 12;

		if (len != want) {
			resp_error(r, WASP_ERR_BAD_ARGS, "malformed request");
			return;
		}
		ref = get_u32(p);
		region = WASP_REF_REGION(ref);
		offset = WASP_REF_OFFSET(ref);
		reg = region_get(ctx, region);
		if (!reg) {
			resp_error(r, WASP_ERR_NO_REGION, NULL);
			return;
		}
		if (!lock_permits(ctx, region, n->owner)) {
			resp_error(r, WASP_ERR_LOCKED, NULL);
			return;
		}
		if (offset + 4 > reg->len) {
			resp_error(r, WASP_ERR_BAD_ARGS, "out of bounds");
			return;
		}
		uint32_t old = get_u32(reg->buf + offset);

		if (rtype == WASP_MSG_MEM_ADD) {
			put_u32(reg->buf + offset, old + get_u32(p + 4));
		} else {
			uint32_t expected = get_u32(p + 4);
			uint32_t desired = get_u32(p + 8);

			if (old == expected)
				put_u32(reg->buf + offset, desired);
		}
		resp_u32(r, WASP_MSG_MEM_DATA, old);
		return;
	}

	/* MEM_READ [ref u32][len u32] / MEM_WRITE [ref u32][data] */
	if (len < (rtype == WASP_MSG_MEM_READ ? 8u : 5u)) {
		resp_error(r, WASP_ERR_BAD_ARGS, "malformed request");
		return;
	}
	ref = get_u32(p);
	region = WASP_REF_REGION(ref);
	offset = WASP_REF_OFFSET(ref);
	reg = region_get(ctx, region);
	if (!reg) {
		resp_error(r, WASP_ERR_NO_REGION, NULL);
		return;
	}
	if (!lock_permits(ctx, region, n->owner)) {
		resp_error(r, WASP_ERR_LOCKED, NULL);
		return;
	}
	if (rtype == WASP_MSG_MEM_READ) {
		uint32_t rdlen = get_u32(p + 4);

		if ((uint64_t)offset + rdlen > reg->len) {
			resp_error(r, WASP_ERR_BAD_ARGS, "out of bounds");
			return;
		}
		if (rdlen > n->rx_cap) {
			resp_error(r, WASP_ERR_TOO_LARGE, NULL);
			return;
		}
		/* p is n->rx: the 8 request bytes are parsed, reuse it. */
		memcpy(n->rx, reg->buf + offset, rdlen);
		r->type = WASP_MSG_MEM_DATA;
		r->payload = n->rx;
		r->len = rdlen;
		return;
	}
	/* MEM_WRITE */
	uint32_t dlen = len - 4;

	if (offset + dlen > reg->len) {
		resp_error(r, WASP_ERR_BAD_ARGS, "out of bounds");
		return;
	}
	memcpy(reg->buf + offset, p + 4, dlen);
	ev_push(ctx, &(wasp_event){ .tag = n->tag,
				    .type = WASP_EV_MEM_WRITE,
				    .region = (uint8_t)region,
				    .offset = offset,
				    .len = dlen });
	resp_empty(r, WASP_MSG_MEM_ACK);
}

int wasp_host_serve(wasp_node *n, uint8_t rtype, uint8_t rseq,
		    uint8_t *payload, uint32_t len)
{
	struct resp r = { 0 };

	wasp_mutex_lock(&n->ctx->mem_mu);
	node_response(n, rtype, payload, len, &r);
	wasp_mutex_unlock(&n->ctx->mem_mu);
	return wasp_send_frame(n->fd, r.type, rseq, r.payload, r.len);
}
