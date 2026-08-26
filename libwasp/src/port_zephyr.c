/*
 * libwasp Zephyr port: k_mutex/k_condvar + zsock sockets. Compiled by
 * the app build (CONFIG_WASP_PEER), never by the POSIX Makefile.
 * Hosts are dotted-quad IPv4 only — nodes have no DNS resolver.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "port.h"
#include "wasp/libwasp.h"

void wasp_mutex_init(wasp_mutex_t *m)
{
	k_mutex_init(m);
}

void wasp_mutex_destroy(wasp_mutex_t *m)
{
	ARG_UNUSED(m);
}

void wasp_mutex_lock(wasp_mutex_t *m)
{
	k_mutex_lock(m, K_FOREVER);
}

void wasp_mutex_unlock(wasp_mutex_t *m)
{
	k_mutex_unlock(m);
}

void wasp_cond_init(wasp_cond_t *c)
{
	k_condvar_init(c);
}

void wasp_cond_destroy(wasp_cond_t *c)
{
	ARG_UNUSED(c);
}

void wasp_cond_signal(wasp_cond_t *c)
{
	k_condvar_signal(c);
}

void wasp_cond_broadcast(wasp_cond_t *c)
{
	k_condvar_broadcast(c);
}

void wasp_cond_timedwait_ms(wasp_cond_t *c, wasp_mutex_t *m,
			    uint32_t timeout_ms)
{
	k_condvar_wait(c, m, K_MSEC(timeout_ms));
}

uint64_t wasp_now_ms(void)
{
	return (uint64_t)k_uptime_get();
}

int wasp_net_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};
	struct zsock_timeval tv = {
		.tv_sec = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000,
	};
	int fd, rc, nodelay = 1;

	if (zsock_inet_pton(AF_INET, host, &addr.sin_addr) != 1)
		return -1;
	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0)
		return -1;

	/* Bound the connect itself via SO_SNDTIMEO (honored by Zephyr's
	 * TCP connect), then leave both timeouts on the socket. */
	zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	rc = zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		zsock_close(fd);
		return -1;
	}
	zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
			 sizeof(nodelay));
	return fd;
}

int wasp_net_send(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = zsock_send(fd, p, len, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return WASP_E_TIMEOUT;
			return WASP_E_CONN;
		}
		p += n;
		len -= (size_t)n;
	}
	return WASP_OK;
}

int wasp_net_recv_exact(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = zsock_recv(fd, p, len, 0);

		if (n == 0)
			return WASP_E_CONN;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return WASP_E_TIMEOUT;
			return WASP_E_CONN;
		}
		p += n;
		len -= (size_t)n;
	}
	return WASP_OK;
}

int wasp_net_readable(int fd, uint32_t timeout_ms)
{
	struct zsock_pollfd pfd = { .fd = fd, .events = ZSOCK_POLLIN };
	int rc = zsock_poll(&pfd, 1, (int)timeout_ms);

	/* HUP/ERR also count: the pending recv will surface the error. */
	return rc > 0 && pfd.revents != 0 ? 1 : 0;
}

void wasp_net_shutdown(int fd)
{
	/* Zephyr only implements SHUT_RD (SHUT_WR/RDWR -> ENOTSUP).
	 * That is all wasp_node_kill needs: recv sees EOF, requests
	 * fail with E_CONN. No FIN reaches the peer — like a genuine
	 * node loss, the other end finds out via its own timeout. */
	zsock_shutdown(fd, ZSOCK_SHUT_RD);
}

void wasp_net_close(int fd)
{
	zsock_close(fd);
}

int wasp_udp_open(uint16_t port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(port),
	};
	int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	int one = 1;

	if (fd < 0)
		return WASP_E_SYS;
	zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		zsock_close(fd);
		return WASP_E_SYS;
	}
	return fd;
}

int wasp_udp_recv(int fd, void *buf, size_t cap, uint32_t timeout_ms,
		  char *ip, size_t ip_cap)
{
	struct sockaddr_in src;
	socklen_t slen = sizeof(src);
	ssize_t len;

	if (!wasp_net_readable(fd, timeout_ms))
		return WASP_E_TIMEOUT;
	len = zsock_recvfrom(fd, buf, cap, 0, (struct sockaddr *)&src, &slen);
	if (len < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
			       ? WASP_E_TIMEOUT
			       : WASP_E_SYS;
	if (ip && ip_cap > 0 &&
	    !zsock_inet_ntop(AF_INET, &src.sin_addr, ip, ip_cap))
		return WASP_E_SYS;
	return (int)len;
}
