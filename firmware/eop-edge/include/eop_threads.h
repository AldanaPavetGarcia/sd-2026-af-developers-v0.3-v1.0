/**
 * @file eop_threads.h
 * @brief Entry points of the application threads (ADR-014).
 * @details main() creates both threads and hands each one its context through the first thread
 *          argument; there is no global state shared between them.
 */

#ifndef EOP_THREADS_H
#define EOP_THREADS_H

#include "eop_link.h"
#include "eop_queue.h"
#include "eop_status.h"
#include "eop_telemetry.h"

/** What a thread needs; one instance, owned by main(). */
struct eop_context {
	const struct eop_device_config *config; /**< Identity and broker. */
	struct eop_queue *queue;                /**< Samples from sensor to MQTT. */
	struct eop_status_board *status;        /**< Shared status. */
	struct eop_link *link;                  /**< Network link (simulated on native_sim). */
};

/**
 * @brief Sensor thread: one sample every CONFIG_EOP_SAMPLE_PERIOD_MS into the queue (US-401 AC1).
 * @param ctx struct eop_context *.
 * @param unused2 Unused.
 * @param unused3 Unused.
 */
void eop_sensor_thread(void *ctx, void *unused2, void *unused3);

/**
 * @brief MQTT thread: owns the broker connection and publishes the queued samples with QoS 1,
 *        reconnecting with exponential backoff (US-401 AC2, AC5). After a reconnection it
 *        flushes the buffer and publishes RECONNECTED (US-402 AC2, AC6); every
 *        CONFIG_EOP_HEALTH_PERIOD_MS it publishes a health report (US-402 AC8).
 * @param ctx struct eop_context *.
 * @param unused2 Unused.
 * @param unused3 Unused.
 */
void eop_mqtt_thread(void *ctx, void *unused2, void *unused3);

#endif /* EOP_THREADS_H */
