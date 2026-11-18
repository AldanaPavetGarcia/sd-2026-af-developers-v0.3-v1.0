#include <zephyr/ztest.h>

#include "eop_watchdog.h"

#define TIMEOUT_MS 5000
#define T0         1000
/* Just below 2^32 ms: the next check-ins wrap around (49.7 days of uptime). */
#define NEAR_WRAP  0xFFFFF000LL

ZTEST_SUITE(watchdog, NULL, NULL, NULL, NULL, NULL);

ZTEST(watchdog, test_nothing_overdue_within_the_timeout)
{
	struct eop_watchdog wdt;

	eop_wdt_init(&wdt, TIMEOUT_MS, T0);
	zassert_equal(eop_wdt_overdue(&wdt, T0), 0);
	zassert_equal(eop_wdt_overdue(&wdt, T0 + TIMEOUT_MS), 0, "exactly the timeout is fine");
	zassert_equal(eop_wdt_restarts(&wdt, EOP_THREAD_SENSOR), 0);
	zassert_equal(eop_wdt_restarts(&wdt, EOP_THREAD_MQTT), 0);
}

/* US-403 AC1, AC3: only the thread that stopped checking in is overdue. */
ZTEST(watchdog, test_only_the_silent_thread_is_overdue)
{
	struct eop_watchdog wdt;

	eop_wdt_init(&wdt, TIMEOUT_MS, T0);
	for (int64_t t = T0; t <= T0 + 3 * TIMEOUT_MS; t += 1000) {
		eop_wdt_checkin(&wdt, EOP_THREAD_SENSOR, t);
	}
	zassert_equal(eop_wdt_overdue(&wdt, T0 + 3 * TIMEOUT_MS), BIT(EOP_THREAD_MQTT));

	eop_wdt_checkin(&wdt, EOP_THREAD_MQTT, T0 + 3 * TIMEOUT_MS);
	zassert_equal(eop_wdt_overdue(&wdt, T0 + 3 * TIMEOUT_MS + 1), 0);
	zassert_equal(eop_wdt_overdue(&wdt, T0 + 4 * TIMEOUT_MS + 1),
		      BIT(EOP_THREAD_SENSOR) | BIT(EOP_THREAD_MQTT), "both silent");
}

/* US-403 AC2, AC6: a restart is counted per thread and gives a fresh timeout. */
ZTEST(watchdog, test_restart_counts_and_resets_the_timer)
{
	struct eop_watchdog wdt;
	int64_t late = T0 + TIMEOUT_MS + 1;

	eop_wdt_init(&wdt, TIMEOUT_MS, T0);
	eop_wdt_checkin(&wdt, EOP_THREAD_SENSOR, late);
	zassert_equal(eop_wdt_overdue(&wdt, late), BIT(EOP_THREAD_MQTT));
	eop_wdt_restarted(&wdt, EOP_THREAD_MQTT, late);
	zassert_equal(eop_wdt_overdue(&wdt, late), 0);
	zassert_equal(eop_wdt_restarts(&wdt, EOP_THREAD_MQTT), 1);
	zassert_equal(eop_wdt_restarts(&wdt, EOP_THREAD_SENSOR), 0);

	eop_wdt_restarted(&wdt, EOP_THREAD_MQTT, late + TIMEOUT_MS);
	zassert_equal(eop_wdt_restarts(&wdt, EOP_THREAD_MQTT), 2);
}

ZTEST(watchdog, test_survives_the_32_bit_wrap)
{
	struct eop_watchdog wdt;

	eop_wdt_init(&wdt, TIMEOUT_MS, NEAR_WRAP);
	zassert_equal(eop_wdt_overdue(&wdt, NEAR_WRAP + TIMEOUT_MS), 0, "no false alarm at wrap");
	eop_wdt_checkin(&wdt, EOP_THREAD_SENSOR, NEAR_WRAP + TIMEOUT_MS);
	zassert_equal(eop_wdt_overdue(&wdt, NEAR_WRAP + TIMEOUT_MS + 1), BIT(EOP_THREAD_MQTT),
		      "still detected after the wrap");
}

ZTEST(watchdog, test_recent_restart_window)
{
	struct eop_watchdog wdt;
	const uint32_t window = 10000;

	eop_wdt_init(&wdt, TIMEOUT_MS, T0);
	zassert_false(eop_wdt_recent_restart(&wdt, T0, window), "no restart yet");
	eop_wdt_restarted(&wdt, EOP_THREAD_SENSOR, T0);
	zassert_true(eop_wdt_recent_restart(&wdt, T0 + window - 1, window));
	zassert_false(eop_wdt_recent_restart(&wdt, T0 + window, window));
}

/* US-403 AC6: an injected hang is taken exactly once, by the thread it was meant for. */
ZTEST(watchdog, test_hang_request_is_taken_once)
{
	struct eop_watchdog wdt;

	eop_wdt_init(&wdt, TIMEOUT_MS, T0);
	zassert_false(eop_wdt_take_hang(&wdt, EOP_THREAD_MQTT));
	eop_wdt_request_hang(&wdt, EOP_THREAD_MQTT);
	zassert_false(eop_wdt_take_hang(&wdt, EOP_THREAD_SENSOR));
	zassert_true(eop_wdt_take_hang(&wdt, EOP_THREAD_MQTT));
	zassert_false(eop_wdt_take_hang(&wdt, EOP_THREAD_MQTT), "the restarted thread runs on");
}

ZTEST(watchdog, test_thread_names)
{
	zassert_str_equal(eop_thread_name(EOP_THREAD_SENSOR), "sensor");
	zassert_str_equal(eop_thread_name(EOP_THREAD_MQTT), "mqtt");
	zassert_str_equal(eop_thread_name(EOP_THREAD_COUNT), "unknown");
}
