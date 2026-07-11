/*
 * Network handler thread.
 *
 * Owns the network interface and the coordinator-facing TCP socket
 * exclusively. Brings the interface up (DHCP), then serves one
 * coordinator connection at a time: complete frames from the wire go to
 * wasp_rx_q, responses from wasp_tx_q go back onto the wire.
 *
 * Frame parsing / serialization is business logic and intentionally not
 * implemented yet.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp_net, LOG_LEVEL_INF);

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

static void wait_for_network(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		LOG_ERR("no network interface found");
		k_sleep(K_FOREVER);
	}

	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	LOG_INF("starting DHCP");
	net_dhcpv4_start(iface);
	k_sem_take(&ipv4_ready, K_FOREVER);
}

static void serve(int client)
{
	uint8_t buf[256];

	/* TODO(protocol): deframe into struct wasp_msg -> wasp_rx_q, and
	 * drain wasp_tx_q back to the socket. For now just drop the data so
	 * the connection plumbing can be exercised. */
	while (true) {
		ssize_t n = zsock_recv(client, buf, sizeof(buf), 0);

		if (n <= 0) {
			LOG_INF("coordinator disconnected (%zd)", n);
			return;
		}
		LOG_INF("received %zd bytes (frame parsing not implemented yet)", n);
	}
}

static void net_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	wait_for_network();

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
		LOG_INF("coordinator connected");
		serve(client);
		zsock_close(client);
	}
}

K_THREAD_DEFINE(wasp_net_tid, CONFIG_WASP_NET_THREAD_STACK_SIZE, net_thread_fn, NULL, NULL, NULL,
		K_PRIO_PREEMPT(6), 0, 0);
