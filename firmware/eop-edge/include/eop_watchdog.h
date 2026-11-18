/**
 * @file eop_watchdog.h
 * @brief Software watchdog of the application threads (US-403 AC1, AC2, ADR-014).
 * @details Each supervised thread checks in at least once per loop. The supervisor thread asks
 *          which threads are overdue (no check-in for longer than the timeout) and restarts only
 *          those, counting each restart. Times are uptime milliseconds kept in 32 bits: the
 *          comparisons are differences, so they stay right across the 49-day wrap.
 */

#ifndef EOP_WATCHDOG_H
#define EOP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

/** Supervised threads. */
enum eop_thread_id {
	EOP_THREAD_SENSOR, /**< Sensor thread. */
	EOP_THREAD_MQTT,   /**< MQTT thread. */
	EOP_THREAD_COUNT,  /**< Number of supervised threads. */
};

/** Watchdog state of one thread. */
struct eop_wdt_entry {
	atomic_t last_checkin_ms; /**< Uptime of the last check-in, low 32 bits. */
	atomic_t restarts;        /**< Restarts since boot. */
	atomic_t hang_requested; /**< Fault injection: hang at the next fault point (US-403 AC6). */
};

/** Watchdog of every supervised thread. */
struct eop_watchdog {
	struct eop_wdt_entry entry[EOP_THREAD_COUNT]; /**< Indexed by eop_thread_id. */
	uint32_t timeout_ms;                          /**< Longest time between check-ins. */
	atomic_t last_restart_ms;                     /**< Uptime of the last restart. */
	atomic_t any_restart;                         /**< 1 once a thread was restarted. */
};

/**
 * @brief Prepares the watchdog; every thread counts as just checked in.
 * @param wdt Watchdog.
 * @param timeout_ms Longest time between check-ins.
 * @param now_ms Current uptime.
 */
void eop_wdt_init(struct eop_watchdog *wdt, uint32_t timeout_ms, int64_t now_ms);

/**
 * @brief Records that @p id is alive.
 * @param wdt Watchdog.
 * @param id Thread.
 * @param now_ms Current uptime.
 */
void eop_wdt_checkin(struct eop_watchdog *wdt, enum eop_thread_id id, int64_t now_ms);

/**
 * @brief Threads whose last check-in is older than the timeout.
 * @param wdt Watchdog.
 * @param now_ms Current uptime.
 * @return Bit (1 << id) set for every overdue thread.
 */
uint32_t eop_wdt_overdue(struct eop_watchdog *wdt, int64_t now_ms);

/**
 * @brief Records a restart of @p id: counts it and gives the new thread a full timeout.
 * @param wdt Watchdog.
 * @param id Thread.
 * @param now_ms Current uptime.
 */
void eop_wdt_restarted(struct eop_watchdog *wdt, enum eop_thread_id id, int64_t now_ms);

/**
 * @brief Restarts of @p id since boot.
 * @param wdt Watchdog.
 * @param id Thread.
 * @return Count.
 */
uint32_t eop_wdt_restarts(struct eop_watchdog *wdt, enum eop_thread_id id);

/**
 * @brief Whether a thread was restarted less than @p window_ms ago.
 * @param wdt Watchdog.
 * @param now_ms Current uptime.
 * @param window_ms Window.
 * @return true inside the window.
 */
bool eop_wdt_recent_restart(struct eop_watchdog *wdt, int64_t now_ms, uint32_t window_ms);

/**
 * @brief Name of a thread, as logs and the shell print it.
 * @param id Thread.
 * @return "sensor" or "mqtt"; "unknown" for an invalid id.
 */
const char *eop_thread_name(enum eop_thread_id id);

/**
 * @brief Fault injection: makes @p id hang at its next fault point (US-403 AC6).
 * @param wdt Watchdog.
 * @param id Thread.
 */
void eop_wdt_request_hang(struct eop_watchdog *wdt, enum eop_thread_id id);

/**
 * @brief Takes a pending hang request of @p id.
 * @param wdt Watchdog.
 * @param id Thread.
 * @return true once per request.
 */
bool eop_wdt_take_hang(struct eop_watchdog *wdt, enum eop_thread_id id);

#endif /* EOP_WATCHDOG_H */
