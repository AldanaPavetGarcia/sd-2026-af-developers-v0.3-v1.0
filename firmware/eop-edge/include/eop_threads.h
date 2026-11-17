/**
 * @file eop_threads.h
 * @brief Entry points of the application threads and their supervision (ADR-014, US-403).
 * @details main() creates the threads and hands each one its context through the first thread
 *          argument; there is no global state shared between them. The supervisor restarts a
 *          sensor or MQTT thread that stops checking in (eop_watchdog.h).
 */

#ifndef EOP_THREADS_H
#define EOP_THREADS_H

#include <zephyr/kernel.h>

#include "eop_led.h"
#include "eop_link.h"
#include "eop_queue.h"
#include "eop_status.h"
#include "eop_telemetry.h"
#include "eop_watchdog.h"

/** How to (re)create a supervised thread. */
struct eop_thread_slot {
	struct k_thread *thread; /**< Thread object. */
	k_thread_stack_t *stack; /**< Its stack. */
	size_t stack_size;       /**< Usable stack size. */
	k_thread_entry_t entry;  /**< Entry point. */
	int priority;            /**< Priority. */
	const char *name;        /**< Thread name. */
};

/** What a thread needs; one instance, owned by main(). */
struct eop_context {
	const struct eop_device_config *config; /**< Identity and broker. */
	struct eop_queue *queue;                /**< Samples from sensor to MQTT. */
	struct eop_status_board *status;        /**< Shared status. */
	struct eop_link *link;                  /**< Network link (simulated on native_sim). */
	struct eop_watchdog *wdt;               /**< Check-ins and restart counters. */
	struct eop_led *led;                    /**< Status LED. */
	const struct eop_thread_slot *slots;    /**< Supervised threads, by eop_thread_id. */
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

/**
 * @brief Supervisor thread: restarts a thread that misses its check-ins and drives the LED
 *        (US-403 AC1-AC3, AC5).
 * @param ctx struct eop_context *.
 * @param unused2 Unused.
 * @param unused3 Unused.
 */
void eop_supervisor_thread(void *ctx, void *unused2, void *unused3);

/**
 * @brief Creates thread @p id from its slot, with @p ctx as its argument.
 * @param ctx Context.
 * @param id Thread.
 */
void eop_thread_start(struct eop_context *ctx, enum eop_thread_id id);

/**
 * @brief Watchdog check-in of thread @p id; call it at least every CONFIG_EOP_WDT_TIMEOUT_MS.
 * @param ctx Context.
 * @param id Calling thread.
 */
void eop_thread_alive(struct eop_context *ctx, enum eop_thread_id id);

/**
 * @brief Check-in at the top of a thread loop; with CONFIG_EOP_FAULT_INJECT, also where an
 *        injected hang takes effect (US-403 AC6).
 * @param ctx Context.
 * @param id Calling thread.
 */
void eop_fault_point(struct eop_context *ctx, enum eop_thread_id id);

/**
 * @brief Makes the `eop` shell commands use @p ctx.
 * @param ctx Context; must outlive the shell.
 */
void eop_shell_attach(struct eop_context *ctx);

#endif /* EOP_THREADS_H */
