/*
 * libwasp port layer: the only OS surface core.c/host.c may touch.
 * Implemented by port_posix.c (Linux/mac coordinators, the Python
 * binding) and port_zephyr.c (node firmware).
 */
#ifndef WASP_PORT_H_
#define WASP_PORT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __ZEPHYR__
#include <zephyr/kernel.h>
typedef struct k_mutex wasp_mutex_t;
typedef struct k_condvar wasp_cond_t;
#else
#include <pthread.h>
typedef pthread_mutex_t wasp_mutex_t;
typedef pthread_cond_t wasp_cond_t;
#endif

void wasp_mutex_init(wasp_mutex_t *m);
void wasp_mutex_destroy(wasp_mutex_t *m);
void wasp_mutex_lock(wasp_mutex_t *m);
void wasp_mutex_unlock(wasp_mutex_t *m);

void wasp_cond_init(wasp_cond_t *c);
void wasp_cond_destroy(wasp_cond_t *c);
void wasp_cond_signal(wasp_cond_t *c);
void wasp_cond_broadcast(wasp_cond_t *c);
/* Wait with m held (released while waiting); wakes on signal or after
 * timeout_ms, whichever first. Spurious wakeups allowed. */
void wasp_cond_timedwait_ms(wasp_cond_t *c, wasp_mutex_t *m,
			    uint32_t timeout_ms);

uint64_t wasp_now_ms(void);

/*
 * Sockets: fds are opaque ints. connect resolves host (a dotted quad
 * everywhere; also DNS names on POSIX), applies timeout_ms to the
 * connect itself and to all later send/recv, and sets TCP_NODELAY.
 * send/recv transfer the full length or fail: WASP_OK, or
 * WASP_E_TIMEOUT / WASP_E_CONN.
 */
int wasp_net_connect(const char *host, uint16_t port, uint32_t timeout_ms);
int wasp_net_send(int fd, const void *buf, size_t len);
int wasp_net_recv_exact(int fd, void *buf, size_t len);
/* 1 = at least one byte (or EOF/error) is waiting, 0 = nothing within
 * timeout_ms. The nonblocking probe behind wasp_call_poll. */
int wasp_net_readable(int fd, uint32_t timeout_ms);
void wasp_net_shutdown(int fd);
void wasp_net_close(int fd);

/*
 * UDP, for discovery: open a datagram socket bound to port (broadcast
 * reception enabled), receive one datagram with the sender's address
 * rendered as a dotted quad. recv returns the datagram length,
 * WASP_E_TIMEOUT when nothing arrived, or WASP_E_SYS. Close the fd
 * with wasp_net_close.
 */
int wasp_udp_open(uint16_t port);
int wasp_udp_recv(int fd, void *buf, size_t cap, uint32_t timeout_ms,
		  char *ip, size_t ip_cap);

#endif /* WASP_PORT_H_ */
