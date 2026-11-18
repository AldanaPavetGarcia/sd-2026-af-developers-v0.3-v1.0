/*
 * WiFi bring-up of the hardware build (ESP32): joins the network named in prj.conf and waits for
 * an IPv4 address, so SNTP and MQTT have a route. Not built for native_sim, where the host
 * network is always there (ADR-014). main() calls it until it succeeds, so a network that is not
 * there at power-on (router still booting, out of range) delays the device instead of stopping it.
 * If the link is lost later, the driver rejoins on its own (CONFIG_ESP32_WIFI_STA_RECONNECT) while
 * the MQTT thread sees its socket fail, buffers and reconnects with backoff (US-402).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
/* dhcpv4.h uses struct net_if without declaring it. */
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi_link.h"

LOG_MODULE_REGISTER(eop_wifi, LOG_LEVEL_INF);

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT)
#define IPV4_EVENTS (NET_EVENT_IPV4_ADDR_ADD)

struct wifi_link {
	struct net_mgmt_event_callback wifi_cb;
	struct net_mgmt_event_callback ipv4_cb;
	struct k_sem addressed;
	bool started;   /* callbacks registered, semaphore ready */
	bool requested; /* a connect request is in progress or succeeded */
};

static void on_wifi(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	struct wifi_link *link = CONTAINER_OF(cb, struct wifi_link, wifi_cb);
	const struct wifi_status *status = cb->info;

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		int result = status != NULL ? status->status : -EIO;

		if (result == 0) {
			net_dhcpv4_start(iface);
		} else {
			/* The next eop_wifi_connect() asks again. */
			link->requested = false;
			LOG_WRN("joining '%s' failed (%d)", CONFIG_EOP_WIFI_SSID, result);
		}
	}
}

static void on_ipv4(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	struct wifi_link *link = CONTAINER_OF(cb, struct wifi_link, ipv4_cb);

	ARG_UNUSED(iface);
	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		k_sem_give(&link->addressed);
	}
}

int eop_wifi_connect(k_timeout_t timeout)
{
	static struct wifi_link link;
	struct net_if *iface = net_if_get_wifi_sta();
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_EOP_WIFI_SSID,
		.ssid_length = sizeof(CONFIG_EOP_WIFI_SSID) - 1,
		.psk = (const uint8_t *)CONFIG_EOP_WIFI_PSK,
		.psk_length = sizeof(CONFIG_EOP_WIFI_PSK) - 1,
		.security = sizeof(CONFIG_EOP_WIFI_PSK) > 1 ? WIFI_SECURITY_TYPE_PSK
							    : WIFI_SECURITY_TYPE_NONE,
		.band = WIFI_FREQ_BAND_UNKNOWN,
		.channel = WIFI_CHANNEL_ANY,
		.mfp = WIFI_MFP_OPTIONAL,
		.timeout = SYS_FOREVER_MS,
	};

	if (params.ssid_length == 0) {
		LOG_ERR("CONFIG_EOP_WIFI_SSID is not set (README, \"ESP32 build\")");
		return -EINVAL;
	}
	if (iface == NULL) {
		LOG_ERR("no WiFi station interface");
		return -ENODEV;
	}
	/* Called again on every retry at boot: register once, and do not ask again while the driver
	 * is still trying (CONFIG_ESP32_WIFI_STA_RECONNECT retries on its own).
	 */
	if (!link.started) {
		k_sem_init(&link.addressed, 0, 1);
		net_mgmt_init_event_callback(&link.wifi_cb, on_wifi, WIFI_EVENTS);
		net_mgmt_add_event_callback(&link.wifi_cb);
		net_mgmt_init_event_callback(&link.ipv4_cb, on_ipv4, IPV4_EVENTS);
		net_mgmt_add_event_callback(&link.ipv4_cb);
		link.started = true;
	}
	if (net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL) {
		return 0;
	}
	if (!link.requested) {
		int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));

		if (ret < 0 && ret != -EALREADY) {
			LOG_ERR("WiFi connect request to '%s' failed (%d)", CONFIG_EOP_WIFI_SSID,
				ret);
			return ret;
		}
		link.requested = true;
	}
	if (k_sem_take(&link.addressed, timeout) != 0) {
		LOG_WRN("no IPv4 address from '%s' yet", CONFIG_EOP_WIFI_SSID);
		return -ETIMEDOUT;
	}
	LOG_INF("joined '%s'", CONFIG_EOP_WIFI_SSID);
	return 0;
}
