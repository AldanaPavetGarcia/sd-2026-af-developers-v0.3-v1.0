#include "eop_led.h"

#include <zephyr/sys/util.h>

/* One period of each pattern, in ticks of EOP_LED_TICK_MS: lit for `on`, then dark until `period`.
 */
struct blink {
	uint32_t on;
	uint32_t period;
};

static const struct blink BLINKS[] = {
	[EOP_LED_OK] = {.on = 1, .period = 20},
	[EOP_LED_OFFLINE] = {.on = 5, .period = 10},
	[EOP_LED_THREAD_ERROR] = {.on = 1, .period = 2},
};

static const char *const PATTERN_NAMES[] = {
	[EOP_LED_OK] = "ok",
	[EOP_LED_OFFLINE] = "offline",
	[EOP_LED_THREAD_ERROR] = "thread error",
};

bool eop_led_level(enum eop_led_pattern pattern, uint32_t tick)
{
	if ((unsigned int)pattern >= ARRAY_SIZE(BLINKS)) {
		pattern = EOP_LED_THREAD_ERROR;
	}
	return tick % BLINKS[pattern].period < BLINKS[pattern].on;
}

enum eop_led_pattern eop_led_pattern_for(bool connected, bool thread_error)
{
	if (thread_error) {
		return EOP_LED_THREAD_ERROR;
	}
	return connected ? EOP_LED_OK : EOP_LED_OFFLINE;
}

const char *eop_led_pattern_name(enum eop_led_pattern pattern)
{
	if ((unsigned int)pattern >= ARRAY_SIZE(PATTERN_NAMES)) {
		return PATTERN_NAMES[EOP_LED_THREAD_ERROR];
	}
	return PATTERN_NAMES[pattern];
}
