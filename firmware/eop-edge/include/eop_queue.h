/**
 * @file eop_queue.h
 * @brief Telemetry queue between the sensor and the MQTT thread (ADR-014).
 * @details A k_msgq of eop_telemetry. When it is full the oldest sample is discarded so the
 *          newest data is kept, and the discard is counted (US-402 AC3, AC8). Only the sensor
 *          thread puts; only the MQTT thread gets.
 */

#ifndef EOP_QUEUE_H
#define EOP_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "eop_telemetry.h"

/** Queue state. */
struct eop_queue {
	struct k_msgq msgq; /**< Samples in FIFO order. */
	atomic_t dropped;   /**< Samples discarded because the queue was full, since boot. */
	atomic_t newest_id; /**< message_id of the last sample put, 0 before the first. */
};

/**
 * @brief Prepares a queue over caller-provided storage.
 * @param queue Queue to initialise.
 * @param buffer Storage for @p max_samples samples, aligned to 4 bytes.
 * @param max_samples Capacity.
 */
void eop_queue_init(struct eop_queue *queue, char *buffer, uint32_t max_samples);

/**
 * @brief Adds a sample, discarding the oldest one if the queue is full.
 * @param queue Queue.
 * @param sample Sample to add.
 * @return true if an older sample was discarded to make room.
 */
bool eop_queue_put(struct eop_queue *queue, const struct eop_telemetry *sample);

/**
 * @brief Takes the oldest sample.
 * @param queue Queue.
 * @param sample Where the sample is written.
 * @param timeout How long to wait for one.
 * @return 0, or -EAGAIN when none arrived in time.
 */
int eop_queue_get(struct eop_queue *queue, struct eop_telemetry *sample, k_timeout_t timeout);

/**
 * @brief Samples waiting in the queue.
 * @param queue Queue.
 * @return Number of samples.
 */
uint32_t eop_queue_count(struct eop_queue *queue);

/**
 * @brief Samples discarded since boot because the queue was full.
 * @param queue Queue.
 * @return Number of samples.
 */
uint32_t eop_queue_dropped(struct eop_queue *queue);

/**
 * @brief message_id of the last sample put, whether still queued or not.
 * @param queue Queue.
 * @return The message_id, 0 before the first put.
 */
uint32_t eop_queue_newest_id(struct eop_queue *queue);

#endif /* EOP_QUEUE_H */
