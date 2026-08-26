/* Network bring-up shared by the worker and conductor roles. */
#ifndef WASP_NETUP_H_
#define WASP_NETUP_H_

#include <zephyr/kernel.h>

/*
 * Bring the default interface up (WiFi join where applicable + DHCP)
 * and block until it has an IPv4 address. Drives the status LED and
 * gives wasp_net_ready on completion. Call from exactly one thread:
 * the net thread on worker builds, the conductor thread otherwise.
 */
void wasp_net_up(void);

/* Given once the interface has an address; other threads (announce,
 * peer smoke) wait on it before touching the network. */
extern struct k_sem wasp_net_ready;

#endif /* WASP_NETUP_H_ */
