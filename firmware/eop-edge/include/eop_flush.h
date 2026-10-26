/**
 * @file eop_flush.h
 * @brief Counts the buffered samples delivered after a reconnection (US-402 AC2, AC6).
 * @details When the broker connection comes back, every sample already taken by the sensor is
 *          "buffered": the newest of them marks the end of the flush. The MQTT thread reports each
 *          acknowledged sample; once the one that ends the flush is delivered it publishes
 *          RECONNECTED with the count. Samples are delivered in message_id order, so the flush
 *          also ends correctly when the buffer overflowed and the oldest samples are gone.
 */

#ifndef EOP_FLUSH_H
#define EOP_FLUSH_H

#include <stdbool.h>
#include <stdint.h>

/** Flush in progress after a reconnection. */
struct eop_flush {
	bool active;        /**< A flush is waiting for its last sample. */
	uint32_t last_id;   /**< message_id of the newest buffered sample. */
	uint32_t delivered; /**< Buffered samples acknowledged so far. */
};

/**
 * @brief Starts counting after a reconnection.
 * @param flush Flush state.
 * @param backlog Whether any sample is waiting (pending or queued).
 * @param last_id message_id of the newest sample taken by the sensor.
 * @return true if there is nothing to flush: RECONNECTED can be sent now, with 0.
 */
bool eop_flush_start(struct eop_flush *flush, bool backlog, uint32_t last_id);

/**
 * @brief Reports a sample acknowledged by the broker.
 * @param flush Flush state.
 * @param message_id Its message_id.
 * @return true if it ended the flush: RECONNECTED is due, with flush->delivered.
 */
bool eop_flush_delivered(struct eop_flush *flush, uint32_t message_id);

#endif /* EOP_FLUSH_H */
