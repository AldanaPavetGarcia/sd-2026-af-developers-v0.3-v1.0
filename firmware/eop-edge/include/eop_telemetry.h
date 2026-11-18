/**
 * @file eop_telemetry.h
 * @brief Telemetry samples, device identity and their JSON encoding (US-401 AC1, AC4).
 * @details The payloads are the contract of docs/edge-telemetry-protocol-v1.0.md §2.1 (telemetry),
 *          §2.2 (events) and §2.3 (health). Temperatures are kept in tenths of a degree so that
 *          encoding needs no floating point printf.
 */

#ifndef EOP_TELEMETRY_H
#define EOP_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Longest node id or bunker name (protocol §2). */
#define EOP_NAME_MAX_LEN  63
/** Hex digits of a boot id (protocol §2). */
#define EOP_BOOT_ID_LEN   8
/** Length of an RFC 3339 timestamp with milliseconds, "2026-11-02T14:03:07.120Z". */
#define EOP_TIMESTAMP_LEN 24
/** Longest broker host name. */
#define EOP_HOST_MAX_LEN  253

/** Node ids with this prefix belong to the HPC engine (ADR-022). */
#define EOP_RESERVED_NODE_PREFIX "hpc-engine"

/** Device status reported with every sample (protocol §2.1). */
enum eop_status {
	EOP_STATUS_OK,
	EOP_STATUS_WARN,
	EOP_STATUS_ALERT,
};

/** One sample, as the sensor thread queues it. */
struct eop_telemetry {
	uint32_t message_id;    /**< 1, 2, 3 … within a boot. */
	int64_t sampled_at_ms;  /**< Wall clock at sampling, milliseconds since the Unix epoch. */
	int16_t temperature_dc; /**< Temperature in tenths of a degree C. */
	enum eop_status status; /**< Status derived from the temperature. */
};

/** What identifies the device and where it publishes; fixed after boot. */
struct eop_device_config {
	char node_id[EOP_NAME_MAX_LEN + 1];     /**< MQTT client id and topic segment. */
	char bunker_name[EOP_NAME_MAX_LEN + 1]; /**< Readable name. */
	char boot_id[EOP_BOOT_ID_LEN + 1];      /**< Random per boot, lowercase hex. */
	char broker_host[EOP_HOST_MAX_LEN + 1]; /**< Broker host name or address. */
	uint16_t broker_port;                   /**< Broker TCP port. */
};

/**
 * @brief Name of a status as it travels in the payload.
 * @param status Status to name.
 * @return "OK", "WARN" or "ALERT"; "OK" for an unknown value.
 */
const char *eop_status_name(enum eop_status status);

/**
 * @brief Status for a temperature, using the configured thresholds.
 * @param temperature_dc Temperature in tenths of a degree C.
 * @param warn_dc Temperature from which the status is WARN.
 * @param alert_dc Temperature from which the status is ALERT.
 * @return ALERT at or above @p alert_dc, WARN at or above @p warn_dc, OK below.
 */
enum eop_status eop_status_for(int16_t temperature_dc, int16_t warn_dc, int16_t alert_dc);

/**
 * @brief Checks a node id: [a-z0-9-]{1,63}, not starting with "hpc-engine".
 * @param node_id NUL-terminated string.
 * @return true if valid.
 */
bool eop_node_id_valid(const char *node_id);

/**
 * @brief Checks a bunker name: [A-Za-z0-9_.-]{1,63} (no spaces, it travels in key=value lines).
 * @param bunker_name NUL-terminated string.
 * @return true if valid.
 */
bool eop_bunker_name_valid(const char *bunker_name);

/**
 * @brief Writes @p epoch_ms as RFC 3339 UTC with milliseconds ("2026-11-02T14:03:07.120Z").
 * @param epoch_ms Milliseconds since the Unix epoch; must not be negative.
 * @param buf Output buffer.
 * @param len Size of @p buf; at least EOP_TIMESTAMP_LEN + 1.
 * @return Characters written (EOP_TIMESTAMP_LEN), -EINVAL for a negative time or -ENOSPC.
 */
int eop_format_timestamp(int64_t epoch_ms, char *buf, size_t len);

/**
 * @brief Encodes a sample as the telemetry JSON of the protocol (§2.1).
 * @param sample Sample to encode.
 * @param config Identity of the device.
 * @param buf Output buffer.
 * @param len Size of @p buf.
 * @return Length of the payload without the terminating NUL, or a negative errno
 *         (-ENOSPC when it does not fit, -EINVAL for a sample before the epoch).
 * @pre The identity in @p config is valid.
 */
int eop_telemetry_encode(const struct eop_telemetry *sample, const struct eop_device_config *config,
			 char *buf, size_t len);

/** Device events (protocol §2.2). */
enum eop_event {
	EOP_EVENT_BOOT,        /**< First broker connection since boot. */
	EOP_EVENT_RECONNECTED, /**< Connection restored; carries the buffered samples delivered. */
};

/** Health report (protocol §2.3); every counter is since boot. */
struct eop_health {
	uint32_t uptime_s;        /**< Seconds since boot. */
	uint32_t restarts_sensor; /**< Restarts of the sensor thread (US-403). */
	uint32_t restarts_mqtt;   /**< Restarts of the MQTT thread (US-403). */
	uint32_t buffered;        /**< Samples waiting in the buffer. */
	uint32_t dropped; /**< Samples discarded because the buffer was full (US-402 AC8). */
};

/**
 * @brief Encodes an event as the JSON of the protocol (§2.2).
 * @param event Event.
 * @param delivered Buffered samples delivered; only sent with RECONNECTED.
 * @param now_ms Wall clock of the event, milliseconds since the Unix epoch.
 * @param config Identity of the device.
 * @param buf Output buffer.
 * @param len Size of @p buf.
 * @return Length of the payload, or -ENOSPC / -EINVAL as eop_telemetry_encode().
 */
int eop_event_encode(enum eop_event event, uint32_t delivered, int64_t now_ms,
		     const struct eop_device_config *config, char *buf, size_t len);

/**
 * @brief Encodes a health report as the JSON of the protocol (§2.3).
 * @param health Report.
 * @param now_ms Wall clock of the report, milliseconds since the Unix epoch.
 * @param config Identity of the device.
 * @param buf Output buffer.
 * @param len Size of @p buf.
 * @return Length of the payload, or -ENOSPC / -EINVAL as eop_telemetry_encode().
 */
int eop_health_encode(const struct eop_health *health, int64_t now_ms,
		      const struct eop_device_config *config, char *buf, size_t len);

#endif /* EOP_TELEMETRY_H */
