/* libwasp discovery: listen for node ANNOUNCE UDP broadcasts. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "internal.h"

int wasp_discover(wasp_info *out, uint32_t max, uint32_t timeout_ms,
		  uint16_t udp_port)
{
	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(udp_port ? udp_port : WASP_ANNOUNCE_PORT),
	};
	struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
	uint64_t deadline = wasp_now_ms() + timeout_ms;
	uint32_t count = 0;
	int fd, one = 1;

	if (!out || max == 0)
		return WASP_E_ARG;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return WASP_E_SYS;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		close(fd);
		return WASP_E_SYS;
	}

	while (wasp_now_ms() < deadline) {
		/* ['W']['A'][ANNOUNCE][version][features][port u16 LE]
		 * [busy u8][board_len u8][board name] */
		uint8_t pkt[512];
		struct sockaddr_in src;
		socklen_t slen = sizeof(src);
		ssize_t len = recvfrom(fd, pkt, sizeof(pkt), 0,
				       (struct sockaddr *)&src, &slen);
		char ip[46];
		wasp_info *info = NULL;
		uint32_t i, board_len;

		if (len < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EINTR)
				continue;
			break;
		}
		if (len < 9 || pkt[0] != WASP_PROTO_MAGIC0 ||
		    pkt[1] != WASP_PROTO_MAGIC1 ||
		    pkt[2] != WASP_MSG_ANNOUNCE)
			continue;
		if (!inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip)))
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
		strncpy(info->ip, ip, sizeof(info->ip) - 1);
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
	close(fd);
	return (int)count;
}
