/**
 * @file eop_backoff.h
 * @brief Exponential backoff between broker reconnection attempts (US-401 AC5, US-402 AC5).
 * @details Delays double from an initial value up to a cap: with 1 s and 30 s the sequence is
 *          1, 2, 4, 8, 16, 30, 30 … seconds. Pure arithmetic, no kernel calls, so it is unit
 *          tested on its own.
 */

#ifndef EOP_BACKOFF_H
#define EOP_BACKOFF_H

#include <stdint.h>

/**
 * @brief Backoff state of one connection.
 * @invariant initial_s <= next_s <= max_s.
 */
struct eop_backoff {
	uint32_t initial_s; /**< First delay. */
	uint32_t max_s;     /**< Largest delay. */
	uint32_t next_s;    /**< Delay returned by the next call to eop_backoff_next(). */
};

/**
 * @brief Prepares a backoff that starts at @p initial_s and never exceeds @p max_s.
 * @param backoff State to initialise.
 * @param initial_s First delay in seconds; 0 is treated as 1.
 * @param max_s Largest delay in seconds; raised to @p initial_s if smaller.
 * @post eop_backoff_next() returns the initial delay.
 */
void eop_backoff_init(struct eop_backoff *backoff, uint32_t initial_s, uint32_t max_s);

/**
 * @brief Returns the delay to wait before the next attempt and doubles the following one.
 * @param backoff State of the connection.
 * @return Delay in seconds, between the initial delay and the cap.
 */
uint32_t eop_backoff_next(struct eop_backoff *backoff);

/**
 * @brief Starts the sequence again, after a successful connection.
 * @param backoff State of the connection.
 * @post eop_backoff_next() returns the initial delay.
 */
void eop_backoff_reset(struct eop_backoff *backoff);

#endif /* EOP_BACKOFF_H */
