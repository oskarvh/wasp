/*
 * Peer-role smoke test: this node acts as the COORDINATOR toward
 * another node — the step-2 proof that libwasp runs in firmware.
 *
 * A boot-time thread dials CONFIG_WASP_PEER_SMOKE_HOST, pushes the
 * remote-memory test module from our own flash, and calls into it;
 * the module's wasp.* RPCs come back over the same socket and are
 * served from a region in OUR ram by the libwasp host engine. The
 * worker runs stock firmware — it cannot tell us from a Linux
 * coordinator.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "internal.h" /* libwasp structs + static-allocation API */

LOG_MODULE_REGISTER(wasp_peer, LOG_LEVEL_INF);

static const uint8_t remote_module_wasm[] = {
#include <remote_module.wasm.inc>
};

static wasp_ctx peer_ctx;
static wasp_node worker;
static uint8_t worker_rx[CONFIG_WASP_PEER_RX_BUF];
static uint8_t region1[64]; /* served to the worker as region 1 */

static int failures;

static void check(const char *what, bool ok)
{
	if (ok) {
		LOG_INF("  [ok] %s", what);
	} else {
		LOG_ERR("  [FAIL] %s", what);
		failures++;
	}
}

static void smoke_run(void)
{
	int32_t r[WASP_CALL_MAX_RESULTS];
	uint32_t ref = wasp_pack_ref(1, 0);
	uint32_t maxp = 0;
	uint8_t ver = 0, feat = 0;
	bool ok;
	int rc;

	for (uint32_t i = 0; i < 16; i++) {
		sys_put_le32(i, &region1[4 * i]);
	}
	wasp_region_register(&peer_ctx, 1, region1, sizeof(region1));

	rc = wasp_hello(&worker, &ver, &maxp, &feat);
	check("HELLO handshake", rc == WASP_OK && ver == WASP_PROTO_VERSION);
	LOG_INF("worker: proto v%u, max payload %u, features 0x%02x", ver, maxp, feat);

	rc = wasp_load(&worker, remote_module_wasm, sizeof(remote_module_wasm));
	if (rc == -(int)WASP_ERR_BUSY) {
		/* Stale module from an earlier (crashed) coordinator. */
		wasp_unload(&worker);
		rc = wasp_load(&worker, remote_module_wasm, sizeof(remote_module_wasm));
	}
	check("LOAD from our flash", rc == WASP_OK);
	if (rc != WASP_OK) {
		return;
	}

	rc = wasp_call(&worker, "region_len", (int32_t[]){ 1 }, 1, r);
	check("REGION_INFO served from our table", rc == 1 && r[0] == 64);

	rc = wasp_call(&worker, "sum_region_bulk", (int32_t[]){ (int32_t)ref, 16 }, 2, r);
	check("bulk MEM_READ of our ram (sum 120)", rc == 1 && r[0] == 120);

	rc = wasp_call(&worker, "fill_region", (int32_t[]){ (int32_t)ref, 16, 7 }, 3, r);
	check("remote MEM_WRITE accepted", rc == 1 && r[0] == 0);
	ok = true;
	for (uint32_t i = 0; i < 16; i++) {
		ok = ok && sys_get_le32(&region1[4 * i]) == 7;
	}
	check("write landed in OUR ram", ok);

	/* The module's atomic_add export returns the NEW value. */
	rc = wasp_call(&worker, "atomic_add", (int32_t[]){ (int32_t)ref, 8 }, 2, r);
	check("MEM_ADD (7 + 8 -> 15)", rc == 1 && r[0] == 15 && sys_get_le32(region1) == 15);

	rc = wasp_call(&worker, "locked_add", (int32_t[]){ 1, (int32_t)ref, 5 }, 3, r);
	check("LOCK/UNLOCK round-trip (15 + 5)", rc == 1 && r[0] == 20);

	rc = wasp_unload(&worker);
	check("UNLOAD", rc == WASP_OK);
}

static void peer_smoke_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Let our own DHCP finish and the worker come up. */
	k_sleep(K_SECONDS(8));
	wasp_ctx_init(&peer_ctx);

	LOG_INF("peer smoke: dialing worker %s", CONFIG_WASP_PEER_SMOKE_HOST);

	int rc = WASP_E_CONN;

	for (int attempt = 1; attempt <= 20 && rc != WASP_OK; attempt++) {
		rc = wasp_node_connect_static(&worker, &peer_ctx, worker_rx,
					      sizeof(worker_rx),
					      CONFIG_WASP_PEER_SMOKE_HOST, 4242,
					      5000);
		if (rc != WASP_OK) {
			LOG_WRN("connect attempt %d failed, retrying", attempt);
			k_sleep(K_SECONDS(3));
		}
	}
	if (rc != WASP_OK) {
		LOG_ERR("PEER SMOKE: FAIL (worker unreachable)");
		return;
	}

	smoke_run();
	wasp_node_free(&worker); /* static node: just closes the socket */

	if (failures == 0) {
		LOG_INF("PEER SMOKE: PASS — this node coordinated %s",
			CONFIG_WASP_PEER_SMOKE_HOST);
	} else {
		LOG_ERR("PEER SMOKE: FAIL (%d check(s))", failures);
	}
}

K_THREAD_DEFINE(wasp_peer_smoke_tid, CONFIG_WASP_PEER_THREAD_STACK_SIZE,
		peer_smoke_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);
