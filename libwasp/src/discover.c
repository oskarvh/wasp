/*
 * libwasp discovery: listen for node ANNOUNCE UDP broadcasts. OS-free
 * (all socket work goes through port.h), so nodes with the peer role
 * can discover their workers the same way a Linux coordinator does.
 */
#include <string.h>

#include "internal.h"

int wasp_discover(wasp_info *out, uint32_t max, uint32_t timeout_ms,
		  uint16_t udp_port)
{
	uint64_t deadline = wasp_now_ms() + timeout_ms;
	uint32_t count = 0;
	int fd;

	if (!out || max == 0)
		return WASP_E_ARG;
	fd = wasp_udp_open(udp_port ? udp_port : WASP_ANNOUNCE_PORT);
	if (fd < 0)
		return WASP_E_SYS;

	while (wasp_now_ms() < deadline) {
		/* ['W']['A'][ANNOUNCE][version][features][port u16 LE]
		 * [busy u8][board_len u8][board name] */
		uint8_t pkt[512];
		char ip[46];
		wasp_info *info = NULL;
		uint32_t i, board_len, wait;
		int len;

		wait = (uint32_t)(deadline - wasp_now_ms());
		if (wait > 500)
			wait = 500;
		len = wasp_udp_recv(fd, pkt, sizeof(pkt), wait, ip, sizeof(ip));
		if (len == WASP_E_TIMEOUT)
			continue;
		if (len < 0)
			break;
		if (len < 9 || pkt[0] != WASP_PROTO_MAGIC0 ||
		    pkt[1] != WASP_PROTO_MAGIC1 ||
		    pkt[2] != WASP_MSG_ANNOUNCE)
			continue;

		for (i = 0; i < count; i++) {
			if (strcmp(out[i].ip, ip) == 0) {
				info = &out[i];
				break;
			}
		}
		if (!info) {
			if (count == max)
				continue;
			info = &out[count++];
		}
		memset(info, 0, sizeof(*info));
		memcpy(info->ip, ip, strlen(ip)); /* both 46 B, nul from memset */
		info->version = pkt[3];
		info->features = pkt[4];
		info->tcp_port = (uint16_t)(pkt[5] | (pkt[6] << 8));
		info->busy = pkt[7] != 0;
		board_len = pkt[8];
		if (board_len > (uint32_t)len - 9)
			board_len = (uint32_t)len - 9;
		if (board_len > sizeof(info->board) - 1)
			board_len = sizeof(info->board) - 1;
		memcpy(info->board, pkt + 9, board_len);
	}
	wasp_net_close(fd);
	return (int)count;
}
