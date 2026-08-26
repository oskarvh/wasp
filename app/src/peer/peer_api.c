/*
 * Peer host-function API implementation: static handle tables over
 * the libwasp core. See peer_api.h for the contract; step 4 registers
 * these as WAMR natives so a WASM module can be the conductor.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

#include "internal.h" /* libwasp static-allocation API */
#include "peer_api.h"

#define PEER_MAX CONFIG_WASP_PEER_MAX_PEERS
#define PEER_CONNECT_TIMEOUT_MS 5000
/* Bounds a blocking request, not a whole async call: while a worker
 * computes, wasp_peer_poll just reports E_AGAIN without waiting. */
#define PEER_REQUEST_TIMEOUT_MS 10000

static wasp_ctx peer_ctx;
static int inited;

static wasp_info found[PEER_MAX];
static int found_count;

static wasp_node conns[PEER_MAX];
static uint8_t conn_rx[PEER_MAX][CONFIG_WASP_PEER_RX_BUF];
static wasp_info conn_info[PEER_MAX];
static uint8_t conn_used[PEER_MAX];

void wasp_peer_init(void)
{
	if (!inited) {
		wasp_ctx_init(&peer_ctx);
		inited = 1;
	}
}

/* Is ip one of our own interface addresses? A discovering node hears
 * its own ANNOUNCE broadcasts (when it also runs the worker role). */
static int is_self(const char *ip)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL || iface->config.ip.ipv4 == NULL) {
		return 0;
	}
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;
		char buf[NET_IPV4_ADDR_LEN];

		if (!a->is_used) {
			continue;
		}
		if (net_addr_ntop(AF_INET, &a->address.in_addr, buf, sizeof(buf)) &&
		    strcmp(buf, ip) == 0) {
			return 1;
		}
	}
	return 0;
}

int wasp_peer_discover(uint32_t wait_ms)
{
	wasp_info all[PEER_MAX + 1];
	int n;

	wasp_peer_init();
	n = wasp_discover(all, ARRAY_SIZE(all), wait_ms, 0);
	if (n < 0) {
		return n;
	}
	found_count = 0;
	for (int i = 0; i < n && found_count < PEER_MAX; i++) {
		if (!is_self(all[i].ip)) {
			found[found_count++] = all[i];
		}
	}
	return found_count;
}

const wasp_info *wasp_peer_info(int idx)
{
	return idx >= 0 && idx < found_count ? &found[idx] : NULL;
}

int wasp_peer_connect_addr(const char *ip, uint16_t port)
{
	int h, rc;

	wasp_peer_init();
	for (h = 0; h < PEER_MAX; h++) {
		if (!conn_used[h]) {
			break;
		}
	}
	if (h == PEER_MAX) {
		return WASP_E_ARG;
	}
	rc = wasp_node_connect_static(&conns[h], &peer_ctx, conn_rx[h],
				      sizeof(conn_rx[h]), ip, port,
				      PEER_REQUEST_TIMEOUT_MS);
	if (rc != WASP_OK) {
		return rc;
	}
	memset(&conn_info[h], 0, sizeof(conn_info[h]));
	strncpy(conn_info[h].ip, ip, sizeof(conn_info[h].ip) - 1);
	conn_info[h].tcp_port = port;
	wasp_node_set_owner(&conns[h], ip);
	wasp_node_set_tag(&conns[h], (uint32_t)h);
	conn_used[h] = 1;
	return h;
}

int wasp_peer_connect(int idx)
{
	const wasp_info *info = wasp_peer_info(idx);
	int h;

	if (info == NULL) {
		return WASP_E_ARG;
	}
	h = wasp_peer_connect_addr(info->ip, info->tcp_port);
	if (h >= 0) {
		conn_info[h] = *info;
	}
	return h;
}

static wasp_node *conn(int h)
{
	return h >= 0 && h < PEER_MAX && conn_used[h] ? &conns[h] : NULL;
}

void wasp_peer_close(int h)
{
	if (conn(h) != NULL) {
		wasp_node_free(&conns[h]); /* static node: closes the fd */
		conn_used[h] = 0;
	}
}

void wasp_peer_kill(int h)
{
	if (conn(h) != NULL) {
		wasp_node_kill(&conns[h]);
	}
}

/* Valid for closed handles too (last known peer), until slot reuse. */
const wasp_info *wasp_peer_conn_info(int h)
{
	return h >= 0 && h < PEER_MAX ? &conn_info[h] : NULL;
}

int wasp_peer_hello(int h, uint8_t *version, uint32_t *max_payload,
		    uint8_t *features)
{
	wasp_node *n = conn(h);

	return n ? wasp_hello(n, version, max_payload, features) : WASP_E_ARG;
}

int wasp_peer_load(int h, const void *wasm, uint32_t len)
{
	wasp_node *n = conn(h);
	int rc;

	if (n == NULL) {
		return WASP_E_ARG;
	}
	rc = wasp_load(n, wasm, len);
	if (rc == -(int)WASP_ERR_BUSY) {
		/* Stale module from an earlier (crashed) coordinator. */
		wasp_unload(n);
		rc = wasp_load(n, wasm, len);
	}
	return rc;
}

int wasp_peer_unload(int h)
{
	wasp_node *n = conn(h);

	return n ? wasp_unload(n) : WASP_E_ARG;
}

int wasp_peer_call(int h, const char *fn, const int32_t *argv, uint32_t argc,
		   int32_t results[WASP_CALL_MAX_RESULTS])
{
	wasp_node *n = conn(h);

	return n ? wasp_call(n, fn, argv, argc, results) : WASP_E_ARG;
}

int wasp_peer_call_async(int h, const char *fn, const int32_t *argv,
			 uint32_t argc)
{
	wasp_node *n = conn(h);

	return n ? wasp_call_async(n, fn, argv, argc) : WASP_E_ARG;
}

int wasp_peer_poll(int h, int32_t results[WASP_CALL_MAX_RESULTS])
{
	wasp_node *n = conn(h);

	return n ? wasp_call_poll(n, results) : WASP_E_ARG;
}

int wasp_export_region(uint8_t region_id, void *buf, uint32_t len)
{
	wasp_peer_init();
	return wasp_region_register(&peer_ctx, region_id, buf, len);
}

int wasp_unexport_region(uint8_t region_id)
{
	wasp_peer_init();
	return wasp_region_unregister(&peer_ctx, region_id);
}

int wasp_peer_poll_event(wasp_event *ev, uint32_t timeout_ms)
{
	wasp_peer_init();
	return wasp_poll_event(&peer_ctx, ev, timeout_ms);
}
