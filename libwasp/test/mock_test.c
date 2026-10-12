/*
 * libwasp test: a scripted fake node over localhost TCP. Covers the
 * handshake, every remote-memory request type (bounds, locks, lease
 * expiry, atomics), queued locks across two connections, ERROR
 * passthrough, write events, and stream-violation detection — no
 * hardware needed.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wasp/libwasp.h"

/* Internal but exported; handy for speaking the protocol as the node. */
int wasp_send_frame(int fd, uint8_t type, uint8_t seq, const void *payload,
		    uint32_t len);

static int failures;

#define CHECK(cond, name)                                                     \
	do {                                                                  \
		if (cond) {                                                   \
			printf("  [ok] %s\n", name);                          \
		} else {                                                      \
			printf("  [FAIL] %s (%s:%d)\n", name, __FILE__,       \
			       __LINE__);                                     \
			failures++;                                           \
		}                                                             \
	} while (0)

/* ---- fake-node helpers --------------------------------------------- */

static int fk_recv(int fd, uint8_t *type, uint8_t *seq, uint8_t *buf,
		   uint32_t cap, uint32_t *len)
{
	uint8_t hdr[8];
	uint32_t need, got = 0;

	for (got = 0; got < 8;) {
		ssize_t n = recv(fd, hdr + got, 8 - got, 0);

		if (n <= 0)
			return -1;
		got += (uint32_t)n;
	}
	assert(hdr[0] == 'W' && hdr[1] == 'A');
	need = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
	       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
	assert(need <= cap);
	for (got = 0; got < need;) {
		ssize_t n = recv(fd, buf + got, need - got, 0);

		if (n <= 0)
			return -1;
		got += (uint32_t)n;
	}
	*type = hdr[2];
	*seq = hdr[3];
	*len = need;
	return 0;
}

/* Send a node-initiated request, return the coordinator's response. */
static void fk_rpc(int fd, uint8_t *nseq, uint8_t type, const void *payload,
		   uint32_t len, uint8_t *rtype, uint8_t *rbuf, uint32_t rcap,
		   uint32_t *rlen)
{
	uint8_t seq = (*nseq)++;
	uint8_t rseq;

	wasp_send_frame(fd, type, seq, payload, len);
	assert(fk_recv(fd, rtype, &rseq, rbuf, rcap, rlen) == 0);
	assert(rseq == seq);
}

/* Wait for a CALL_INST of the given export; returns its seq. */
static uint8_t fk_expect_call_inst(int fd, const char *name, uint8_t *inst)
{
	uint8_t type, seq, buf[256];
	uint32_t len;

	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_CALL_INST);
	*inst = buf[0];
	assert(buf[1] == strlen(name) && memcmp(buf + 2, name, buf[1]) == 0);
	return seq;
}

/* Wait for a CALL of the given export; returns its seq. */
static uint8_t fk_expect_call(int fd, const char *name)
{
	uint8_t type, seq, buf[256];
	uint32_t len;

	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_CALL);
	assert(buf[0] == strlen(name) && memcmp(buf + 1, name, buf[0]) == 0);
	return seq;
}

static void fk_result_i32(int fd, uint8_t seq, int32_t v)
{
	uint8_t p[5] = { 1, (uint8_t)v, (uint8_t)((uint32_t)v >> 8),
			 (uint8_t)((uint32_t)v >> 16),
			 (uint8_t)((uint32_t)v >> 24) };

	wasp_send_frame(fd, WASP_MSG_RESULT, seq, p, sizeof(p));
}

static void le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- the scripted fake node ---------------------------------------- */

static int lock_granted_to_b; /* queued-lock scenario verdict */

static void *fake_main(void *arg)
{
	int fd = (int)(intptr_t)arg;
	uint8_t nseq = 0x80, type, seq, buf[512];
	uint32_t len;

	/* HELLO -> HELLO_ACK v1, max 32768, features 0x03 */
	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_HELLO && len == 1 &&
	       buf[0] == WASP_PROTO_VERSION);
	uint8_t ack[6] = { WASP_PROTO_VERSION, 0, 0x80, 0, 0, 0x03 };

	wasp_send_frame(fd, WASP_MSG_HELLO_ACK, seq, ack, sizeof(ack));

	/* CALL go: the full remote-memory volley mid-call. */
	uint8_t call_seq = fk_expect_call(fd, "go");
	uint8_t req[16], rtype, rbuf[256];
	uint32_t rlen;

	/* REGION_INFO(1) -> 64; REGION_INFO(9) -> NO_REGION */
	req[0] = 1;
	fk_rpc(fd, &nseq, WASP_MSG_REGION_INFO, req, 1, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_REGION_DESC && rlen == 4 && rbuf[0] == 64);
	req[0] = 9;
	fk_rpc(fd, &nseq, WASP_MSG_REGION_INFO, req, 1, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_ERROR && rbuf[0] == WASP_ERR_NO_REGION);

	/* MEM_READ whole region: expect 0..15 as LE i32s. */
	le32(req, wasp_pack_ref(1, 0));
	le32(req + 4, 64);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_READ, req, 8, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_MEM_DATA && rlen == 64);
	for (int i = 0; i < 16; i++)
		assert(rbuf[4 * i] == i && rbuf[4 * i + 1] == 0);

	/* Out-of-bounds read. */
	le32(req, wasp_pack_ref(1, 40));
	le32(req + 4, 64);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_READ, req, 8, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_ERROR && rbuf[0] == WASP_ERR_BAD_ARGS);

	/* MEM_WRITE 8 x 0xAA at offset 4. */
	le32(req, wasp_pack_ref(1, 4));
	memset(req + 4, 0xAA, 8);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_WRITE, req, 12, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_MEM_ACK);

	/* MEM_ADD +5 at offset 0 (old 0), CAS 5->60 win, CAS 5->99 lose. */
	le32(req, wasp_pack_ref(1, 0));
	le32(req + 4, 5);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_ADD, req, 8, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_MEM_DATA && rlen == 4 && rbuf[0] == 0);
	le32(req + 4, 5);
	le32(req + 8, 60);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_CAS, req, 12, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_MEM_DATA && rbuf[0] == 5);
	le32(req + 4, 5);
	le32(req + 8, 99);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_CAS, req, 12, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_MEM_DATA && rbuf[0] == 60); /* lost */

	/* LOCK / UNLOCK round-trip. */
	req[0] = 1;
	fk_rpc(fd, &nseq, WASP_MSG_LOCK, req, 1, &rtype, rbuf, sizeof(rbuf),
	       &rlen);
	assert(rtype == WASP_MSG_LOCK_GRANT && rlen == 4);
	fk_rpc(fd, &nseq, WASP_MSG_UNLOCK, req, 1, &rtype, rbuf, sizeof(rbuf),
	       &rlen);
	assert(rtype == WASP_MSG_MEM_ACK);

	fk_result_i32(fd, call_seq, 42);

	/* CALL locked: a rival lease blocks, then expires. */
	call_seq = fk_expect_call(fd, "locked");
	le32(req, wasp_pack_ref(1, 0));
	memset(req + 4, 0x55, 4);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_WRITE, req, 8, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_ERROR && rbuf[0] == WASP_ERR_LOCKED);
	usleep(250 * 1000); /* outlive the 150 ms rival lease */
	fk_rpc(fd, &nseq, WASP_MSG_MEM_WRITE, req, 8, &rtype, rbuf,
	       sizeof(rbuf), &rlen);
	assert(rtype == WASP_MSG_MEM_ACK);
	fk_result_i32(fd, call_seq, 1);

	/* CALL boom -> ERROR(TRAP, "boom"). */
	call_seq = fk_expect_call(fd, "boom");
	uint8_t trap[5] = { WASP_ERR_TRAP, 'b', 'o', 'o', 'm' };

	wasp_send_frame(fd, WASP_MSG_ERROR, call_seq, trap, sizeof(trap));

	/* PING answered with a wrong seq: stream violation. */
	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_PING);
	wasp_send_frame(fd, WASP_MSG_PONG, (uint8_t)(seq + 1), NULL, 0);

	close(fd);
	return NULL;
}

/* Queued-lock scenario: A holds the lock 300 ms, B parks and is
 * granted (fail-fast would see ERROR(LOCKED) instead). */
static void *fake_hold(void *arg)
{
	int fd = (int)(intptr_t)arg;
	uint8_t nseq = 0x10, rtype, rbuf[64], req[1] = { 1 };
	uint32_t rlen;
	uint8_t call_seq = fk_expect_call(fd, "hold");

	fk_rpc(fd, &nseq, WASP_MSG_LOCK, req, 1, &rtype, rbuf, sizeof(rbuf),
	       &rlen);
	assert(rtype == WASP_MSG_LOCK_GRANT);
	usleep(300 * 1000);
	fk_rpc(fd, &nseq, WASP_MSG_UNLOCK, req, 1, &rtype, rbuf, sizeof(rbuf),
	       &rlen);
	assert(rtype == WASP_MSG_MEM_ACK);
	fk_result_i32(fd, call_seq, 1);
	close(fd);
	return NULL;
}

static void *fake_wait(void *arg)
{
	int fd = (int)(intptr_t)arg;
	uint8_t nseq = 0x20, rtype, rbuf[64], req[1] = { 1 };
	uint32_t rlen;
	uint8_t call_seq = fk_expect_call(fd, "wait");

	usleep(100 * 1000); /* let A take the lock first */
	fk_rpc(fd, &nseq, WASP_MSG_LOCK, req, 1, &rtype, rbuf, sizeof(rbuf),
	       &rlen);
	lock_granted_to_b = rtype == WASP_MSG_LOCK_GRANT;
	if (lock_granted_to_b) {
		fk_rpc(fd, &nseq, WASP_MSG_UNLOCK, req, 1, &rtype, rbuf,
		       sizeof(rbuf), &rlen);
		assert(rtype == WASP_MSG_MEM_ACK);
	}
	fk_result_i32(fd, call_seq, 1);
	close(fd);
	return NULL;
}

/* Async scenario: answer CALL slow with one MEM_ADD mid-call, then a
 * RESULT — with delays, so the coordinator's poll loop actually spins. */
/*
 * A node running two instances at once. It takes a call on each, answers
 * the second while the first is still working, and only then issues the
 * first one's remote-memory RPC and completes it — so the coordinator has
 * to cope with responses arriving in the opposite order to the requests,
 * which is exactly what a node whose instances run concurrently does.
 */
static void *fake_async(void *arg)
{
	int fd = (int)(intptr_t)arg;
	uint8_t nseq = 0x30, rtype, rbuf[64], req[8];
	uint32_t rlen;
	uint8_t slow_inst = 0, fast_inst = 0;
	uint8_t slow_seq = fk_expect_call_inst(fd, "slow", &slow_inst);
	uint8_t fast_seq = fk_expect_call_inst(fd, "fast", &fast_inst);

	/* The second call finishes first. */
	usleep(20 * 1000);
	fk_result_i32(fd, fast_seq, 11);

	/* Now the first one's remote access, then its result. */
	usleep(50 * 1000);
	le32(req, wasp_pack_ref(1, 32));
	le32(req + 4, 9);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_ADD, req, 8, &rtype, rbuf, sizeof(rbuf),
	       &rlen);
	assert(rtype == WASP_MSG_MEM_DATA);
	usleep(20 * 1000);
	fk_result_i32(fd, slow_seq, 7);
	close(fd);
	return NULL;
}

/* A node answering the synchronous instance API. */
static void *fake_instances(void *arg)
{
	int fd = (int)(intptr_t)arg;
	uint8_t type, seq, buf[512];
	uint32_t len;
	uint8_t reply[16];

	/* MODULE_UPLOAD [n_inst][.wasm] -> [mod][n][inst x n] */
	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_MODULE_UPLOAD);
	assert(buf[0] == 2);              /* asked for two instances */
	assert(len == 1 + 4);             /* one byte of count + 4 of "wasm" */
	reply[0] = WASP_HANDLE(0, 0);     /* module 0, generation 0 */
	reply[1] = 2;
	reply[2] = WASP_HANDLE(0, 0);
	reply[3] = WASP_HANDLE(1, 0);
	wasp_send_frame(fd, WASP_MSG_RESULT, seq, reply, 4);

	/* INSTANTIATE [mod][count] -> [count][inst x count] */
	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_INSTANTIATE && len == 2);
	assert(buf[0] == WASP_HANDLE(0, 0) && buf[1] == 1);
	reply[0] = 1;
	reply[1] = WASP_HANDLE(2, 3); /* slot 2, third generation */
	wasp_send_frame(fd, WASP_MSG_RESULT, seq, reply, 2);

	/* CALL_INST on that handle */
	{
		uint8_t inst = 0;

		seq = fk_expect_call_inst(fd, "add", &inst);
		assert(inst == WASP_HANDLE(2, 3));
		fk_result_i32(fd, seq, 42);
	}

	/* INVENTORY -> capacity plus what is resident */
	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_INVENTORY);
	reply[0] = 3;                 /* slots_total */
	reply[1] = 1;                 /* slots_free */
	le32(reply + 2, 0x30000);     /* pool_free */
	reply[6] = 1;                 /* one module */
	reply[7] = WASP_HANDLE(0, 0);
	reply[8] = 2;
	reply[9] = WASP_HANDLE(0, 0);
	reply[10] = WASP_HANDLE(1, 0);
	wasp_send_frame(fd, WASP_MSG_RESULT, seq, reply, 11);

	/* DROP a stale handle -> BAD_HANDLE */
	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_DROP && len == 2);
	assert(buf[0] == WASP_DROP_INSTANCE);
	reply[0] = WASP_ERR_BAD_HANDLE;
	wasp_send_frame(fd, WASP_MSG_ERROR, seq, reply, 1);

	/* DROP the module -> empty RESULT */
	assert(fk_recv(fd, &type, &seq, buf, sizeof(buf), &len) == 0);
	assert(type == WASP_MSG_DROP && buf[0] == WASP_DROP_MODULE);
	wasp_send_frame(fd, WASP_MSG_RESULT, seq, NULL, 0);

	close(fd);
	return NULL;
}

/* ---- coordinator-side driver --------------------------------------- */

static uint16_t start_listener(int *lfd)
{
	struct sockaddr_in sa = { .sin_family = AF_INET };
	socklen_t slen = sizeof(sa);

	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	*lfd = socket(AF_INET, SOCK_STREAM, 0);
	assert(*lfd >= 0);
	assert(bind(*lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(listen(*lfd, 4) == 0);
	assert(getsockname(*lfd, (struct sockaddr *)&sa, &slen) == 0);
	return ntohs(sa.sin_port);
}

struct call_job {
	wasp_node *node;
	const char *fn;
	int rc;
};

static void *call_thread(void *arg)
{
	struct call_job *j = arg;
	int32_t res[WASP_CALL_MAX_RESULTS];

	j->rc = wasp_call(j->node, j->fn, NULL, 0, res);
	return NULL;
}

int main(void)
{
	wasp_ctx *ctx = wasp_ctx_new();
	uint8_t region1[64];
	int lfd;
	uint16_t port = start_listener(&lfd);
	pthread_t fk;

	for (int i = 0; i < 16; i++)
		le32(region1 + 4 * i, (uint32_t)i);
	assert(wasp_region_register(ctx, 1, region1, sizeof(region1)) ==
	       WASP_OK);

	/* -- main scenario ------------------------------------------- */
	wasp_node *node = wasp_connect(ctx, "127.0.0.1", port, 5000);

	CHECK(node != NULL, "connect");
	int afd = accept(lfd, NULL, NULL);

	assert(afd >= 0);
	pthread_create(&fk, NULL, fake_main, (void *)(intptr_t)afd);
	wasp_node_set_tag(node, 77);

	uint8_t ver = 0, feat = 0;
	uint32_t maxp = 0;

	CHECK(wasp_hello(node, &ver, &maxp, &feat) == WASP_OK &&
		      ver == 1 && maxp == 32768 && feat == 0x03,
	      "hello handshake");

	int32_t res[WASP_CALL_MAX_RESULTS];
	int rc = wasp_call(node, "go", NULL, 0, res);

	CHECK(rc == 1 && res[0] == 42, "call with inline RPC volley");
	CHECK(region1[4] == 0xAA && region1[11] == 0xAA,
	      "MEM_WRITE landed in the region");
	CHECK(region1[0] == 60, "MEM_ADD + winning/losing CAS sequence");

	wasp_event ev;

	CHECK(wasp_poll_event(ctx, &ev, 100) == 1 &&
		      ev.type == WASP_EV_MEM_WRITE && ev.tag == 77 &&
		      ev.region == 1 && ev.offset == 4 && ev.len == 8,
	      "MEM_WRITE event observed");
	CHECK(wasp_poll_event(ctx, &ev, 10) == 0, "no spurious events");

	CHECK(wasp_lock_force(ctx, 1, "rival", 150) == WASP_OK,
	      "force rival lease");
	rc = wasp_call(node, "locked", NULL, 0, res);
	CHECK(rc == 1 && res[0] == 1, "rival lease blocks, then expires");
	CHECK(wasp_lock_holder(ctx, 1, NULL, 0) == 0, "expired lease revoked");
	/* drain the MEM_WRITE event from the locked scenario */
	wasp_poll_event(ctx, &ev, 100);

	rc = wasp_call(node, "boom", NULL, 0, res);
	CHECK(rc == -(int)WASP_ERR_TRAP &&
		      strcmp(wasp_node_error_detail(node), "boom") == 0,
	      "ERROR passthrough with detail");

	uint8_t rtype, rseq;
	const uint8_t *rp;
	uint32_t rl;

	rc = wasp_request(node, WASP_MSG_PING, NULL, 0, &rtype, &rseq, &rp,
			  &rl);
	CHECK(rc == WASP_E_STREAM, "seq mismatch detected");
	pthread_join(fk, NULL);
	wasp_node_free(node);

	/* -- queued locks across two connections ---------------------- */
	wasp_node *na = wasp_connect(ctx, "127.0.0.1", port, 5000);
	int fda = accept(lfd, NULL, NULL);
	wasp_node *nb = wasp_connect(ctx, "127.0.0.1", port, 5000);
	int fdb = accept(lfd, NULL, NULL);

	assert(na && nb && fda >= 0 && fdb >= 0);
	wasp_node_set_owner(na, "A");
	wasp_node_set_owner(nb, "B");
	wasp_node_set_queue_locks(nb, 1);

	pthread_t fka, fkb, ca, cb;
	struct call_job ja = { .node = na, .fn = "hold" };
	struct call_job jb = { .node = nb, .fn = "wait" };

	pthread_create(&fka, NULL, fake_hold, (void *)(intptr_t)fda);
	pthread_create(&fkb, NULL, fake_wait, (void *)(intptr_t)fdb);
	pthread_create(&ca, NULL, call_thread, &ja);
	pthread_create(&cb, NULL, call_thread, &jb);
	pthread_join(ca, NULL);
	pthread_join(cb, NULL);
	pthread_join(fka, NULL);
	pthread_join(fkb, NULL);
	CHECK(ja.rc == 1 && jb.rc == 1, "both queued-lock calls completed");
	CHECK(lock_granted_to_b, "contended LOCK parked and granted (FIFO)");
	CHECK(wasp_lock_holder(ctx, 1, NULL, 0) == 0, "lock free afterwards");

	wasp_node_free(na);
	wasp_node_free(nb);

	/* -- async call: nonblocking poll loop ------------------------ */
	wasp_node *nc = wasp_connect(ctx, "127.0.0.1", port, 5000);
	int fdc = accept(lfd, NULL, NULL);
	pthread_t fkc;

	assert(nc && fdc >= 0);
	pthread_create(&fkc, NULL, fake_async, (void *)(intptr_t)fdc);
	le32(region1 + 32, 100);

	/* Two calls in flight at once, on different instances. The old API
	 * allowed one per node because the node's executor was
	 * single-threaded; it no longer is.
	 */
	CHECK(wasp_call_inst_async(nc, WASP_HANDLE(0, 0), "slow", NULL, 0) ==
	      WASP_OK, "async call to instance 0 sent");
	CHECK(wasp_call_inst_async(nc, WASP_HANDLE(1, 0), "fast", NULL, 0) ==
	      WASP_OK, "second async call to another instance accepted");
	CHECK(wasp_call_inflight(nc) == 2, "two calls outstanding");

	int agains = 0;
	uint8_t done_inst = 0xFF;

	for (;;) {
		rc = wasp_call_poll_any(nc, &done_inst, res);
		if (rc != WASP_E_AGAIN)
			break;
		agains++;
		usleep(5 * 1000);
	}
	/* The node answered the second call first; poll must report which
	 * instance completed, not assume an order. */
	CHECK(rc == 1 && res[0] == 11 && done_inst == WASP_HANDLE(1, 0),
	      "the instance that finished first completes first");
	CHECK(agains > 0, "poll returned E_AGAIN while the calls ran");
	CHECK(wasp_call_inflight(nc) == 1, "the slow call is still outstanding");

	for (;;) {
		rc = wasp_call_poll_any(nc, &done_inst, res);
		if (rc != WASP_E_AGAIN)
			break;
		usleep(5 * 1000);
	}
	CHECK(rc == 1 && res[0] == 7 && done_inst == WASP_HANDLE(0, 0),
	      "the slow instance completes second");
	CHECK(region1[32] == 109, "MEM_ADD served from inside the poll loop");
	CHECK(wasp_call_inflight(nc) == 0, "nothing left outstanding");
	pthread_join(fkc, NULL);
	wasp_node_free(nc);
	/* drain the async scenario's events */
	while (wasp_poll_event(ctx, &ev, 10) == 1)
		;

	/* -- synchronous instance API --------------------------------- */
	wasp_node *nd = wasp_connect(ctx, "127.0.0.1", port, 5000);
	int fdd = accept(lfd, NULL, NULL);
	pthread_t fkd;
	uint8_t mod = 0xFF, insts[WASP_HANDLE_SLOTS], more = 0xFF;
	wasp_inventory inv;

	assert(nd && fdd >= 0);
	pthread_create(&fkd, NULL, fake_instances, (void *)(intptr_t)fdd);

	rc = wasp_upload(nd, "wasm", 4, 2, &mod, insts, sizeof(insts));
	CHECK(rc == 2 && mod == WASP_HANDLE(0, 0), "upload returns two instances");
	CHECK(insts[0] == WASP_HANDLE(0, 0) && insts[1] == WASP_HANDLE(1, 0),
	      "both instance handles decoded");

	rc = wasp_instantiate(nd, mod, 1, &more, 1);
	CHECK(rc == 1 && more == WASP_HANDLE(2, 3),
	      "instantiate decodes a regenerated handle");
	CHECK(WASP_HANDLE_IDX(more) == 2 && WASP_HANDLE_GEN(more) == 3,
	      "handle splits into slot and generation");

	rc = wasp_call_inst(nd, more, "add", NULL, 0, res);
	CHECK(rc == 1 && res[0] == 42, "call on a named instance");

	CHECK(wasp_inventory_get(nd, &inv) == WASP_OK, "inventory read");
	CHECK(inv.slots_total == 3 && inv.slots_free == 1 &&
	      inv.pool_free == 0x30000, "inventory reports capacity");
	CHECK(inv.n_modules == 1 && inv.modules[0].n_inst == 2,
	      "inventory lists the module's instances");

	CHECK(wasp_drop_instance(nd, WASP_HANDLE(5, 1)) == -WASP_ERR_BAD_HANDLE,
	      "a stale handle is refused");
	CHECK(wasp_drop_module(nd, mod) == WASP_OK, "module dropped");

	pthread_join(fkd, NULL);
	wasp_node_free(nd);

	close(lfd);
	wasp_ctx_free(ctx);

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("all libwasp mock tests passed\n");
	return 0;
}
