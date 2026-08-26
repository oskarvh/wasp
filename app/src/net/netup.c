/*
 * Network bring-up, shared by every role: join WiFi where the board
 * has it, start DHCP, and block until the interface has an address.
 * Split out of net_thread.c so a conductor-only build (no worker role,
 * no listening socket) can bring the network up the same way.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#ifdef CONFIG_WIFI
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#endif

#include "led.h"
#include "netup.h"

LOG_MODULE_REGISTER(wasp_netup, LOG_LEVEL_INF);

K_SEM_DEFINE(wasp_net_ready, 0, 1);

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

void wasp_net_up(void)
{
	struct net_if *iface = net_if_get_default();

	wasp_led_set(WASP_LED_NET_DOWN);

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

	wasp_led_set(WASP_LED_READY);
	k_sem_give(&wasp_net_ready);
}
