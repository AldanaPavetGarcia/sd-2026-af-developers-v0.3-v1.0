/**
 * @file eop_led.h
 * @brief Status LED: a blink pattern per device state (US-403 AC5).
 * @details The LED is the devicetree alias `led0`: the emulated GPIO of native_sim, or the LED the
 *          board overlay names on the ESP32. A k_timer advances the pattern every
 *          EOP_LED_TICK_MS; the supervisor chooses the pattern once per check.
 *
 *          | Pattern       | Blink                         | Meaning                            |
 *          |---------------|-------------------------------|------------------------------------|
 *          | OK            | 100 ms on every 2 s           | connected, every thread alive      |
 *          | OFFLINE       | 500 ms on, 500 ms off (1 Hz)  | no broker connection: buffering    |
 *          | THREAD_ERROR  | 100 ms on, 100 ms off (5 Hz)  | a thread was restarted lately      |
 */

#ifndef EOP_LED_H
#define EOP_LED_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/** Length of one pattern step. */
#define EOP_LED_TICK_MS 100

/** LED patterns, by priority: a thread error hides being offline. */
enum eop_led_pattern {
	EOP_LED_OK,
	EOP_LED_OFFLINE,
	EOP_LED_THREAD_ERROR,
};

/** LED state. */
struct eop_led {
	struct k_timer timer; /**< Advances the pattern every EOP_LED_TICK_MS. */
	atomic_t pattern;     /**< Current eop_led_pattern. */
	atomic_t tick;        /**< Steps since the pattern started. */
};

/**
 * @brief Whether the LED is lit at step @p tick of @p pattern.
 * @param pattern Pattern.
 * @param tick Steps of EOP_LED_TICK_MS since the pattern started.
 * @return true when lit.
 */
bool eop_led_level(enum eop_led_pattern pattern, uint32_t tick);

/**
 * @brief Pattern for a device state.
 * @param connected Whether the broker connection is up.
 * @param thread_error Whether a thread was restarted lately.
 * @return The pattern to show.
 */
enum eop_led_pattern eop_led_pattern_for(bool connected, bool thread_error);

/**
 * @brief Name of a pattern, as the shell prints it.
 * @param pattern Pattern.
 * @return "ok", "offline" or "thread error".
 */
const char *eop_led_pattern_name(enum eop_led_pattern pattern);

/**
 * @brief Configures the LED GPIO, if the board has one, and starts the pattern timer.
 * @param led LED state.
 * @return 0, or a negative errno if the GPIO exists but could not be configured.
 */
int eop_led_start(struct eop_led *led);

/**
 * @brief Shows @p pattern from its first step, unless it is already showing.
 * @param led LED state.
 * @param pattern Pattern.
 */
void eop_led_show(struct eop_led *led, enum eop_led_pattern pattern);

/**
 * @brief Pattern being shown.
 * @param led LED state.
 * @return The pattern.
 */
enum eop_led_pattern eop_led_current(struct eop_led *led);

#endif /* EOP_LED_H */
