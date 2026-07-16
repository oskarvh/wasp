/*
 * Node discovery: broadcast an ANNOUNCE datagram every few seconds so
 * coordinators can find nodes without knowing their addresses (see
 * protocol.h for the payload). Runs in its own small thread — the net
 * thread blocks in accept() and could not keep a steady beat.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp_announce, LOG_LEVEL_INF);

#ifdef CONFIG_BOARD_TARGET
#define ANNOUNCE_BOARD CONFIG_BOARD_TARGET
#else
#define ANNOUNCE_BOARD CONFIG_BOARD
#endif

BUILD_ASSERT(sizeof(ANNOUNCE_BOARD) - 1 <= UINT8_MAX);

static void announce_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint8_t pkt[9 + sizeof(ANNOUNCE_BOARD) - 1];
	bool sending = true;

	pkt[0] = WASP_PROTO_MAGIC0;
	pkt[1] = WASP_PROTO_MAGIC1;
	pkt[2] = WASP_MSG_ANNOUNCE;
	pkt[3] = WASP_PROTO_VERSION;
	pkt[4] = WASP_FEAT_REMOTE_MEM | WASP_FEAT_ATOMICS;
	sys_put_le16(CONFIG_WASP_AGENT_PORT, &pkt[5]);
	pkt[8] = sizeof(ANNOUNCE_BOARD) - 1;
	memcpy(&pkt[9], ANNOUNCE_BOARD, sizeof(ANNOUNCE_BOARD) - 1);

	/* Nothing to announce until the network is up. */
	k_sem_take(&wasp_net_ready, K_FOREVER);

	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		LOG_ERR("announce socket failed: %d", errno);
		return;
	}

	int bcast = 1;

	zsock_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

	struct sockaddr_in dest = {
		.sin_family = AF_INET,
		.sin_addr = {.s_addr = htonl(INADDR_BROADCAST)},
		.sin_port = htons(CONFIG_WASP_ANNOUNCE_PORT),
	};

	LOG_INF("announcing on UDP port %d every %d s", CONFIG_WASP_ANNOUNCE_PORT,
		CONFIG_WASP_ANNOUNCE_INTERVAL);

	while (true) {
		pkt[7] = atomic_get(&wasp_node_busy) ? 1 : 0;

		ssize_t n = zsock_sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest,
					 sizeof(dest));

		/* Log transitions only — not one line per beacon. */
		if ((n < 0) == sending) {
			sending = n >= 0;
			if (!sending) {
				LOG_WRN("announce send failed: %d", errno);
			}
		}
		k_sleep(K_SECONDS(CONFIG_WASP_ANNOUNCE_INTERVAL));
	}
}

K_THREAD_DEFINE(wasp_announce_tid, 1536, announce_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(11),
		0, 0);
