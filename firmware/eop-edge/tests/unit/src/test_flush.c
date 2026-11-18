#include <zephyr/ztest.h>

#include "eop_flush.h"

ZTEST_SUITE(flush, NULL, NULL, NULL, NULL, NULL);

ZTEST(flush, test_nothing_buffered_reports_at_once)
{
	struct eop_flush flush;

	zassert_true(eop_flush_start(&flush, false, 41));
	zassert_equal(flush.delivered, 0);
	/* Later samples are new data, not part of a flush. */
	zassert_false(eop_flush_delivered(&flush, 42));
	zassert_equal(flush.delivered, 0);
}

ZTEST(flush, test_ends_on_the_newest_buffered_sample)
{
	struct eop_flush flush;

	zassert_false(eop_flush_start(&flush, true, 30));
	for (uint32_t id = 1; id < 30; id++) {
		zassert_false(eop_flush_delivered(&flush, id), "id %u", id);
	}
	zassert_true(eop_flush_delivered(&flush, 30));
	zassert_equal(flush.delivered, 30);
	zassert_false(eop_flush_delivered(&flush, 31), "only one RECONNECTED per flush");
	zassert_equal(flush.delivered, 30);
}

ZTEST(flush, test_counts_only_what_survived_an_overflow)
{
	struct eop_flush flush;

	/* 100 samples taken while offline, the 40 oldest dropped: 41..100 are delivered. */
	zassert_false(eop_flush_start(&flush, true, 100));
	for (uint32_t id = 41; id < 100; id++) {
		zassert_false(eop_flush_delivered(&flush, id));
	}
	zassert_true(eop_flush_delivered(&flush, 100));
	zassert_equal(flush.delivered, 60);
}

ZTEST(flush, test_ends_if_the_newest_buffered_sample_was_dropped)
{
	struct eop_flush flush;

	zassert_false(eop_flush_start(&flush, true, 10));
	zassert_false(eop_flush_delivered(&flush, 9));
	/* 10 was dropped while flushing: 11 is new data and closes the flush. */
	zassert_true(eop_flush_delivered(&flush, 11));
	zassert_equal(flush.delivered, 1);
}

ZTEST(flush, test_a_new_reconnection_restarts_the_count)
{
	struct eop_flush flush;

	zassert_false(eop_flush_start(&flush, true, 20));
	zassert_false(eop_flush_delivered(&flush, 15));
	/* The connection dropped again before 20 was delivered. */
	zassert_false(eop_flush_start(&flush, true, 25));
	for (uint32_t id = 16; id < 25; id++) {
		zassert_false(eop_flush_delivered(&flush, id));
	}
	zassert_true(eop_flush_delivered(&flush, 25));
	zassert_equal(flush.delivered, 10);
}
