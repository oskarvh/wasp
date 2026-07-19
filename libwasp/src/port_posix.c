/* libwasp POSIX port: pthreads + BSD sockets. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "port.h"
#include "wasp/libwasp.h"

void wasp_mutex_init(wasp_mutex_t *m)
{
	pthread_mutex_init(m, NULL);
}

void wasp_mutex_destroy(wasp_mutex_t *m)
{
	pthread_mutex_destroy(m);
}

void wasp_mutex_lock(wasp_mutex_t *m)
{
	pthread_mutex_lock(m);
}

void wasp_mutex_unlock(wasp_mutex_t *m)
{
	pthread_mutex_unlock(m);
}

void wasp_cond_init(wasp_cond_t *c)
{
	pthread_condattr_t attr;

	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(c, &attr);
	pthread_condattr_destroy(&attr);
}

void wasp_cond_destroy(wasp_cond_t *c)
{
	pthread_cond_destroy(c);
}

void wasp_cond_signal(wasp_cond_t *c)
{
	pthread_cond_signal(c);
}

void wasp_cond_broadcast(wasp_cond_t *c)
{
	pthread_cond_broadcast(c);
}

void wasp_cond_timedwait_ms(wasp_cond_t *c, wasp_mutex_t *m,
			    uint32_t timeout_ms)
{
	uint64_t at = wasp_now_ms() + timeout_ms;
	struct timespec ts = { .tv_sec = (time_t)(at / 1000),
			       .tv_nsec = (long)(at % 1000) * 1000000 };

	pthread_cond_timedwait(c, m, &ts);
}

uint64_t wasp_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int connect_timeout(int fd, const struct sockaddr *sa, socklen_t salen,
			   uint32_t timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLOUT };
	int err = 0;
	socklen_t elen = sizeof(err);
	int flags = fcntl(fd, F_GETFL, 0);

	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (connect(fd, sa, salen) == 0)
		goto done;
	if (errno != EINPROGRESS)
		return WASP_E_CONN;
	if (poll(&pfd, 1, (int)timeout_ms) <= 0)
		return WASP_E_TIMEOUT;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
		return WASP_E_CONN;
done:
	fcntl(fd, F_SETFL, flags);
	return WASP_OK;
}

int wasp_net_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
	struct addrinfo hints = { .ai_family = AF_UNSPEC,
				  .ai_socktype = SOCK_STREAM };
	struct addrinfo *res = NULL, *ai;
	char portstr[8];
	int fd = -1;

	snprintf(portstr, sizeof(portstr), "%u", port);
	if (getaddrinfo(host, portstr, &hints, &res) != 0)
		return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect_timeout(fd, ai->ai_addr, ai->ai_addrlen,
				    timeout_ms) == WASP_OK)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		return -1;

	int one = 1;
	struct timeval tv = { .tv_sec = timeout_ms / 1000,
			      .tv_usec = (timeout_ms % 1000) * 1000 };

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	return fd;
}

int wasp_net_send(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

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
		ssize_t n = recv(fd, p, len, 0);

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

void wasp_net_shutdown(int fd)
{
	shutdown(fd, SHUT_RDWR);
}

void wasp_net_close(int fd)
{
	close(fd);
}
