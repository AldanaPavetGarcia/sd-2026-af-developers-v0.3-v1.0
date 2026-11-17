#include <zephyr/ztest.h>

#include "eop_led.h"

ZTEST_SUITE(led, NULL, NULL, NULL, NULL, NULL);

/* Lit steps in one period of @p period ticks. */
static uint32_t lit(enum eop_led_pattern pattern, uint32_t period)
{
	uint32_t count = 0;

	for (uint32_t tick = 0; tick < period; tick++) {
		count += eop_led_level(pattern, tick) ? 1 : 0;
	}
	return count;
}

/* The three patterns of the README (US-403 AC5) tell apart at a glance. */
ZTEST(led, test_patterns_match_the_readme)
{
	/* OK: 100 ms every 2 s. */
	zassert_true(eop_led_level(EOP_LED_OK, 0));
	zassert_false(eop_led_level(EOP_LED_OK, 1));
	zassert_true(eop_led_level(EOP_LED_OK, 20), "repeats every 2 s");
	zassert_equal(lit(EOP_LED_OK, 20), 1);

	/* OFFLINE: 500 ms on, 500 ms off. */
	zassert_equal(lit(EOP_LED_OFFLINE, 10), 5);
	zassert_true(eop_led_level(EOP_LED_OFFLINE, 4));
	zassert_false(eop_led_level(EOP_LED_OFFLINE, 5));

	/* THREAD_ERROR: 5 Hz. */
	for (uint32_t tick = 0; tick < 10; tick++) {
		zassert_equal(eop_led_level(EOP_LED_THREAD_ERROR, tick), tick % 2 == 0);
	}
}

ZTEST(led, test_unknown_pattern_shows_an_error)
{
	enum eop_led_pattern bogus = (enum eop_led_pattern)9;

	for (uint32_t tick = 0; tick < 4; tick++) {
		zassert_equal(eop_led_level(bogus, tick),
			      eop_led_level(EOP_LED_THREAD_ERROR, tick));
	}
	zassert_str_equal(eop_led_pattern_name(bogus), "thread error");
}

ZTEST(led, test_a_thread_error_hides_being_offline)
{
	zassert_equal(eop_led_pattern_for(true, false), EOP_LED_OK);
	zassert_equal(eop_led_pattern_for(false, false), EOP_LED_OFFLINE);
	zassert_equal(eop_led_pattern_for(false, true), EOP_LED_THREAD_ERROR);
	zassert_equal(eop_led_pattern_for(true, true), EOP_LED_THREAD_ERROR);
	zassert_str_equal(eop_led_pattern_name(EOP_LED_OK), "ok");
	zassert_str_equal(eop_led_pattern_name(EOP_LED_OFFLINE), "offline");
}
