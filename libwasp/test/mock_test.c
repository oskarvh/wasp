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
static void *fake_async(void *arg)
{
	int fd = (int)(intptr_t)arg;
	uint8_t nseq = 0x30, rtype, rbuf[64], req[8];
	uint32_t rlen;
	uint8_t call_seq = fk_expect_call(fd, "slow");

	usleep(100 * 1000);
	le32(req, wasp_pack_ref(1, 32));
	le32(req + 4, 9);
	fk_rpc(fd, &nseq, WASP_MSG_MEM_ADD, req, 8, &rtype, rbuf, sizeof(rbuf),
	       &rlen);
	assert(rtype == WASP_MSG_MEM_DATA);
	usleep(50 * 1000);
	fk_result_i32(fd, call_seq, 7);
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
	CHECK(wasp_call_async(nc, "slow", NULL, 0) == WASP_OK,
	      "async call sent");
	CHECK(wasp_call_async(nc, "slow", NULL, 0) == WASP_E_ARG,
	      "second async call on the slot refused");

	int agains = 0;

	for (;;) {
		rc = wasp_call_poll(nc, res);
		if (rc != WASP_E_AGAIN)
			break;
		agains++;
		usleep(5 * 1000);
	}
	CHECK(rc == 1 && res[0] == 7, "async call completed via poll");
	CHECK(agains > 0, "poll returned E_AGAIN while the call ran");
	CHECK(region1[32] == 109, "MEM_ADD served from inside the poll loop");
	pthread_join(fkc, NULL);
	wasp_node_free(nc);
	/* drain the async scenario's events */
	while (wasp_poll_event(ctx, &ev, 10) == 1)
		;
	close(lfd);
	wasp_ctx_free(ctx);

	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("all libwasp mock tests passed\n");
	return 0;
}
