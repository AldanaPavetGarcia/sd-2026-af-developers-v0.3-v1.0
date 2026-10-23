#include <zephyr/ztest.h>

#include "eop_backoff.h"

ZTEST_SUITE(backoff, NULL, NULL, NULL, NULL, NULL);

/* US-401 AC5: 1 s, 2 s, 4 s, 8 s, max 30 s. */
ZTEST(backoff, test_sequence_doubles_up_to_the_cap)
{
	const uint32_t expected[] = {1, 2, 4, 8, 16, 30, 30, 30};
	struct eop_backoff b;

	eop_backoff_init(&b, 1, 30);
	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		zassert_equal(eop_backoff_next(&b), expected[i], "attempt %zu", i);
	}
}

ZTEST(backoff, test_reset_starts_again)
{
	struct eop_backoff b;

	eop_backoff_init(&b, 1, 30);
	(void)eop_backoff_next(&b);
	(void)eop_backoff_next(&b);
	(void)eop_backoff_next(&b);
	eop_backoff_reset(&b);
	zassert_equal(eop_backoff_next(&b), 1);
	zassert_equal(eop_backoff_next(&b), 2);
}

ZTEST(backoff, test_cap_not_a_power_of_two_is_reached_exactly)
{
	struct eop_backoff b;

	eop_backoff_init(&b, 3, 20);
	zassert_equal(eop_backoff_next(&b), 3);
	zassert_equal(eop_backoff_next(&b), 6);
	zassert_equal(eop_backoff_next(&b), 12);
	zassert_equal(eop_backoff_next(&b), 20);
	zassert_equal(eop_backoff_next(&b), 20);
}

ZTEST(backoff, test_degenerate_limits_are_corrected)
{
	struct eop_backoff b;

	/* A zero initial delay would make the device retry in a tight loop. */
	eop_backoff_init(&b, 0, 30);
	zassert_equal(eop_backoff_next(&b), 1);

	eop_backoff_init(&b, 10, 5);
	zassert_equal(eop_backoff_next(&b), 10);
	zassert_equal(eop_backoff_next(&b), 10);
}

ZTEST(backoff, test_huge_cap_does_not_overflow)
{
	struct eop_backoff b;
	uint32_t last = 0;

	eop_backoff_init(&b, 1, UINT32_MAX);
	for (int i = 0; i < 40; i++) {
		uint32_t next = eop_backoff_next(&b);

		zassert_true(next >= last, "delay went down at attempt %d", i);
		last = next;
	}
	zassert_equal(last, UINT32_MAX);
}
