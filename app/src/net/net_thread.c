/*
 * Network handler thread.
 *
 * Owns the network interface and the coordinator-facing TCP socket
 * exclusively. Brings the interface up (DHCP), then serves one
 * coordinator connection at a time: complete frames from the wire go to
 * wasp_rx_q, responses from wasp_tx_q go back onto the wire.
 *
 * Framing errors are handled here, before the agent ever sees the
 * message: an unknown magic means the stream is desynchronized and the
 * connection is dropped; oversized or unallocatable payloads are drained
 * from the stream and answered with an ERROR frame directly.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#ifdef CONFIG_WIFI
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#endif

#include "led.h"
#include "wasp.h"

LOG_MODULE_REGISTER(wasp_net, LOG_LEVEL_INF);

K_SEM_DEFINE(wasp_net_ready, 0, 1);
atomic_t wasp_node_busy;

/* How often the serve loop wakes to drain wasp_tx_q while idle. */
#define SERVE_POLL_MS 50

/* A peer that stalls mid-frame longer than this is dropped, so a broken
 * coordinator cannot wedge the node's single connection slot. */
#define SOCK_TIMEOUT_SECONDS 10

/* Bumped on every accept; rx messages are stamped with it and tx
 * messages carrying a stale generation are discarded unsent. */
static uint8_t conn_gen;

static struct net_mgmt_event_callback ipv4_cb;
static K_SEM_DEFINE(ipv4_ready, 0, 1);

static void on_ipv4_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			  struct net_if *iface)
{
	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		char addr[NET_IPV4_ADDR_LEN];
		struct net_if_addr *if_addr = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (!if_addr->is_used) {
			continue;
		}
		net_addr_ntop(AF_INET, &if_addr->address.in_addr, addr, sizeof(addr));
		LOG_INF("IPv4 address: %s", addr);
	}
	k_sem_give(&ipv4_ready);
}

#ifdef CONFIG_WIFI

BUILD_ASSERT(sizeof(CONFIG_WASP_WIFI_SSID) > 1,
	     "CONFIG_WASP_WIFI_SSID is empty — copy app/wifi_credentials.conf.example "
	     "to app/wifi_credentials.conf and fill in your network");

static struct net_mgmt_event_callback wifi_cb;
static K_SEM_DEFINE(wifi_result, 0, 1);
static int wifi_conn_status;

static void on_wifi_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			  struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = cb->info;

		wifi_conn_status = status->status;
		k_sem_give(&wifi_result);
	}
}

#if defined(CONFIG_WIFI_AIROC) && !defined(CONFIG_WASP_WIFI_POWERSAVE)

#include <whd_wifi_api.h>

extern whd_interface_t airoc_wifi_get_whd_interface(void);

/*
 * WHD enables chip power save (PM2) at init. The sleep/wake cycle adds
 * beacon-interval latency bursts to every radio transaction — remote
 * memory RPCs, and on the Pico W the status LED (a write is a WHD ioctl,
 * which stalls while the chip sleeps, visibly garbling blink patterns).
 * Nodes are usually mains-powered, so trade the power back for latency.
 */
static void wifi_disable_powersave(void)
{
	whd_interface_t ifp = airoc_wifi_get_whd_interface();

	if (ifp == NULL || whd_wifi_disable_powersave(ifp) != WHD_SUCCESS) {
		LOG_WRN("could not disable WiFi power save");
	} else {
		LOG_INF("WiFi power save disabled");
	}
}

#else
static void wifi_disable_powersave(void)
{
}
#endif /* CONFIG_WIFI_AIROC && !CONFIG_WASP_WIFI_POWERSAVE */

/* Join the configured network; retries forever — a node without a
 * network has nothing else to do. */
static void wifi_connect(struct net_if *iface)
{
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_WASP_WIFI_SSID,
		.ssid_length = strlen(CONFIG_WASP_WIFI_SSID),
		.psk = (const uint8_t *)CONFIG_WASP_WIFI_PSK,
		.psk_length = strlen(CONFIG_WASP_WIFI_PSK),
		.security = strlen(CONFIG_WASP_WIFI_PSK) > 0 ? WIFI_SECURITY_TYPE_PSK
							     : WIFI_SECURITY_TYPE_NONE,
		.band = WIFI_FREQ_BAND_UNKNOWN,
		.channel = WIFI_CHANNEL_ANY,
		.timeout = SYS_FOREVER_MS,
	};

	net_mgmt_init_event_callback(&wifi_cb, on_wifi_event, NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	while (true) {
		LOG_INF("connecting to WiFi \"%s\"", CONFIG_WASP_WIFI_SSID);

		int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));

		if (rc == 0 && k_sem_take(&wifi_result, K_SECONDS(30)) == 0 &&
		    wifi_conn_status == 0) {
			LOG_INF("WiFi connected");
			wifi_disable_powersave();
			return;
		}
		LOG_WRN("WiFi connect failed (rc %d, status %d), retrying in 5 s", rc,
			wifi_conn_status);
		k_sleep(K_SECONDS(5));
	}
}

#endif /* CONFIG_WIFI */

static void wait_for_network(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		LOG_ERR("no network interface found");
		k_sleep(K_FOREVER);
	}

#ifdef CONFIG_WIFI
	wifi_connect(iface);
#endif

	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	LOG_INF("starting DHCP");
	net_dhcpv4_start(iface);
	k_sem_take(&ipv4_ready, K_FOREVER);
}

/* --- socket helpers -------------------------------------------------- */

/* Receive exactly len bytes. Returns 0, or -1 on error/peer close. */
static int recv_full(int sock, uint8_t *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = zsock_recv(sock, buf, len, 0);

		if (n <= 0) {
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}

/* Read and discard len bytes to keep the stream in sync. */
static int drain_bytes(int sock, uint32_t len)
{
	uint8_t scratch[64];

	while (len > 0) {
		size_t chunk = MIN(len, sizeof(scratch));

		if (recv_full(sock, scratch, chunk) < 0) {
			return -1;
		}
		len -= chunk;
	}
	return 0;
}

static int send_full(int sock, const uint8_t *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = zsock_send(sock, buf, len, 0);

		if (n < 0) {
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}

static int send_frame(int sock, const struct wasp_msg *msg)
{
	uint8_t hdr[WASP_FRAME_HDR_SIZE];

	hdr[WASP_FRAME_OFF_MAGIC] = WASP_PROTO_MAGIC0;
	hdr[WASP_FRAME_OFF_MAGIC + 1] = WASP_PROTO_MAGIC1;
	hdr[WASP_FRAME_OFF_TYPE] = msg->type;
	hdr[WASP_FRAME_OFF_SEQ] = msg->seq;
	sys_put_le32(msg->len, &hdr[WASP_FRAME_OFF_LEN]);

	if (send_full(sock, hdr, sizeof(hdr)) < 0) {
		return -1;
	}
	if (msg->len > 0 && send_full(sock, msg->payload, msg->len) < 0) {
		return -1;
	}
	return 0;
}

static int send_error(int sock, uint8_t seq, uint8_t err)
{
	struct wasp_msg msg = {
		.type = WASP_MSG_ERROR,
		.seq = seq,
		.len = 1,
		.payload = &err,
	};

	return send_frame(sock, &msg);
}

/* --- frame rx/tx ------------------------------------------------------ */

/*
 * Receive one frame and hand it to the agent.
 * Returns 0 on success (including handled-and-skipped frames), -1 when
 * the connection must be closed.
 */
static int recv_frame(int sock)
{
	uint8_t hdr[WASP_FRAME_HDR_SIZE];
	struct wasp_msg msg = {0};
	uint32_t len;

	if (recv_full(sock, hdr, sizeof(hdr)) < 0) {
		return -1;
	}

	if (hdr[WASP_FRAME_OFF_MAGIC] != WASP_PROTO_MAGIC0 ||
	    hdr[WASP_FRAME_OFF_MAGIC + 1] != WASP_PROTO_MAGIC1) {
		LOG_WRN("bad magic %02x%02x, dropping connection", hdr[0], hdr[1]);
		return -1;
	}

	msg.type = hdr[WASP_FRAME_OFF_TYPE];
	msg.seq = hdr[WASP_FRAME_OFF_SEQ];
	msg.conn = conn_gen;
	len = sys_get_le32(&hdr[WASP_FRAME_OFF_LEN]);

	if (len > CONFIG_WASP_MAX_MODULE_SIZE) {
		LOG_WRN("frame type 0x%02x len %u exceeds cap %d", msg.type, len,
			CONFIG_WASP_MAX_MODULE_SIZE);
		if (drain_bytes(sock, len) < 0 ||
		    send_error(sock, msg.seq, WASP_ERR_TOO_LARGE) < 0) {
			return -1;
		}
		return 0;
	}

	if (len > 0) {
		msg.payload = k_heap_alloc(&wasp_payload_heap, len, K_MSEC(500));
		if (msg.payload == NULL) {
			LOG_WRN("no memory for %u byte payload", len);
			if (drain_bytes(sock, len) < 0 ||
			    send_error(sock, msg.seq, WASP_ERR_NO_MEM) < 0) {
				return -1;
			}
			return 0;
		}
		if (recv_full(sock, msg.payload, len) < 0) {
			k_heap_free(&wasp_payload_heap, msg.payload);
			return -1;
		}
		msg.len = len;
	}

	if (k_msgq_put(&wasp_rx_q, &msg, K_MSEC(500)) != 0) {
		LOG_WRN("agent queue full, dropping frame type 0x%02x", msg.type);
		if (msg.payload != NULL) {
			k_heap_free(&wasp_payload_heap, msg.payload);
		}
		return send_error(sock, msg.seq, WASP_ERR_BUSY);
	}

	return 0;
}

/*
 * Send queued responses to the coordinator. Messages stamped with a
 * stale connection generation (produced for a peer that is already
 * gone) are discarded unsent; pass sock < 0 to discard everything.
 * Returns 0, or -1 when the connection must be closed.
 */
static int flush_tx(int sock)
{
	struct wasp_msg msg;
	int rc = 0;

	while (k_msgq_get(&wasp_tx_q, &msg, K_NO_WAIT) == 0) {
		if (msg.conn != conn_gen) {
			LOG_WRN("discarding stale response type 0x%02x (conn %u, now %u)",
				msg.type, msg.conn, conn_gen);
		} else if (sock >= 0 && rc == 0) {
			rc = send_frame(sock, &msg);
		}
		if (msg.payload != NULL) {
			k_heap_free(&wasp_payload_heap, msg.payload);
		}
	}
	return rc;
}

static void serve(int client)
{
	struct zsock_pollfd pfd = {
		.fd = client,
		.events = ZSOCK_POLLIN,
	};

	while (true) {
		int rc = zsock_poll(&pfd, 1, SERVE_POLL_MS);

		if (rc < 0) {
			LOG_ERR("poll() failed: %d", errno);
			return;
		}
		if (pfd.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
			return;
		}
		if ((pfd.revents & ZSOCK_POLLIN) && recv_frame(client) < 0) {
			return;
		}
		if (flush_tx(client) < 0) {
			return;
		}
	}
}

static void net_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	wasp_led_set(WASP_LED_NET_DOWN);
	wait_for_network();
	wasp_led_set(WASP_LED_READY);
	k_sem_give(&wasp_net_ready);

	int server = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (server < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return;
	}

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_addr = {.s_addr = htonl(INADDR_ANY)},
		.sin_port = htons(CONFIG_WASP_AGENT_PORT),
	};

	if (zsock_bind(server, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0 ||
	    zsock_listen(server, 1) < 0) {
		LOG_ERR("bind/listen failed: %d", errno);
		zsock_close(server);
		return;
	}

	LOG_INF("listening for coordinator on port %d", CONFIG_WASP_AGENT_PORT);

	while (true) {
		int client = zsock_accept(server, NULL, NULL);

		if (client < 0) {
			LOG_ERR("accept() failed: %d", errno);
			continue;
		}

		conn_gen++;

		struct zsock_timeval timeout = {.tv_sec = SOCK_TIMEOUT_SECONDS};

		zsock_setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		zsock_setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

		atomic_set(&wasp_node_busy, 1);
		wasp_led_set(WASP_LED_CONNECTED);

		LOG_INF("coordinator connected");
		serve(client);
		zsock_close(client);
		flush_tx(-1);
		LOG_INF("coordinator disconnected");

		atomic_set(&wasp_node_busy, 0);
		wasp_led_set(WASP_LED_READY);
	}
}

K_THREAD_DEFINE(wasp_net_tid, CONFIG_WASP_NET_THREAD_STACK_SIZE, net_thread_fn, NULL, NULL, NULL,
		K_PRIO_PREEMPT(6), 0, 0);
