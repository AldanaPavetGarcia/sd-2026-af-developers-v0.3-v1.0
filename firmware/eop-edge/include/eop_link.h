/**
 * @file eop_link.h
 * @brief State of the device's network link, as the MQTT thread sees it (US-402).
 * @details On native_sim the host network never goes away, so the link is a switch the shell
 *          command `eop net down|up` flips (ADR-014): while it is down the MQTT thread closes its
 *          connection and does not reconnect, and the sensor keeps filling the buffer. When the
 *          link comes back the MQTT thread is woken at once instead of finishing its backoff
 *          sleep, as a real "network connected" event would.
 */

#ifndef EOP_LINK_H
#define EOP_LINK_H

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/** Link state. */
struct eop_link {
	atomic_t up;          /**< 1 while the link is up. */
	struct k_sem came_up; /**< Given on every down → up change. */
};

/**
 * @brief Prepares a link.
 * @param link Link to initialise.
 * @param up Initial state.
 */
void eop_link_init(struct eop_link *link, bool up);

/**
 * @brief Changes the link state.
 * @param link Link.
 * @param up New state.
 * @return true if the state changed.
 */
bool eop_link_set(struct eop_link *link, bool up);

/**
 * @brief Whether the link is up.
 * @param link Link.
 * @return true while up.
 */
bool eop_link_is_up(struct eop_link *link);

/**
 * @brief Waits for @p timeout, returning early if the link comes up meanwhile.
 * @param link Link.
 * @param timeout Longest wait.
 * @return true if the link came up during the wait.
 */
bool eop_link_wait_up(struct eop_link *link, k_timeout_t timeout);

/**
 * @brief Forgets earlier down → up changes, so the next wait is a full one.
 * @param link Link.
 */
void eop_link_clear(struct eop_link *link);

#endif /* EOP_LINK_H */
