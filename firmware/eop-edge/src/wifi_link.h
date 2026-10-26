/*
 * WiFi bring-up of the hardware build (src/wifi_link.c); only compiled when CONFIG_WIFI is set.
 */

#ifndef EOP_WIFI_LINK_H
#define EOP_WIFI_LINK_H

#include <zephyr/kernel.h>

/* Joins CONFIG_EOP_WIFI_SSID and waits for an IPv4 address; 0 or a negative errno. */
int eop_wifi_connect(k_timeout_t timeout);

#endif /* EOP_WIFI_LINK_H */
