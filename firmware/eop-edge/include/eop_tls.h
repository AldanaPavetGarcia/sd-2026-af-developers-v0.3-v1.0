/**
 * @file eop_tls.h
 * @brief TLS to the broker's device listener (ADR-013), built with tls.conf.
 * @details The device authenticates the broker: its certificate must chain to the CA built into
 *          the firmware (-DEOP_MQTT_CA_CERT) and name the broker host, or
 *          CONFIG_EOP_MQTT_TLS_HOSTNAME when set. The device has no certificate of its own.
 */

#ifndef EOP_TLS_H
#define EOP_TLS_H

#include <stddef.h>

#include <zephyr/net/tls_credentials.h>

/** Security tag of the broker's CA in the TLS credential store. */
#define EOP_TLS_CA_TAG 1

/**
 * @brief Adds the broker's CA to the TLS credential store.
 * @return 0 on success (also when it was already added), a negative errno otherwise.
 * @post Sockets configured with EOP_TLS_CA_TAG verify the broker against that CA.
 */
int eop_tls_init(void);

#endif /* EOP_TLS_H */
