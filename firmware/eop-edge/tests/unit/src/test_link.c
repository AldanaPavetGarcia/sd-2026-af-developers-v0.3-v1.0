#include <zephyr/ztest.h>

#include "eop_link.h"

#define SHORT_WAIT     K_MSEC(20)
#define LONG_WAIT      K_SECONDS(5)
#define FLIP_DELAY     50
#define WOKEN_LIMIT_MS 1000

ZTEST_SUITE(link, NULL, NULL, NULL, NULL, NULL);

ZTEST(link, test_reports_changes_only)
{
	struct eop_link link;

	eop_link_init(&link, true);
	zassert_true(eop_link_is_up(&link));
	zassert_false(eop_link_set(&link, true), "already up");
	zassert_true(eop_link_set(&link, false));
	zassert_false(eop_link_is_up(&link));
	zassert_false(eop_link_set(&link, false), "already down");
	zassert_true(eop_link_set(&link, true));
	zassert_true(eop_link_is_up(&link));
}

ZTEST(link, test_a_wait_without_changes_times_out)
{
	struct eop_link link;

	eop_link_init(&link, false);
	zassert_false(eop_link_wait_up(&link, SHORT_WAIT));
	/* Going down does not wake the waiter either. */
	eop_link_init(&link, true);
	eop_link_set(&link, false);
	zassert_false(eop_link_wait_up(&link, SHORT_WAIT));
}

static struct eop_link shared_link;
K_THREAD_STACK_DEFINE(flip_stack, 1024);
static struct k_thread flip_thread;

static void bring_up_later(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);
	k_msleep(FLIP_DELAY);
	eop_link_set(&shared_link, true);
}

ZTEST(link, test_coming_up_cuts_the_wait_short)
{
	eop_link_init(&shared_link, false);
	k_thread_create(&flip_thread, flip_stack, K_THREAD_STACK_SIZEOF(flip_stack), bring_up_later,
			NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);

	int64_t started = k_uptime_get();

	zassert_true(eop_link_wait_up(&shared_link, LONG_WAIT));
	zassert_true(k_uptime_get() - started < WOKEN_LIMIT_MS,
		     "woken by the change, not the timeout");
	k_thread_join(&flip_thread, K_FOREVER);
}

ZTEST(link, test_clear_forgets_an_earlier_change)
{
	struct eop_link link;

	eop_link_init(&link, false);
	eop_link_set(&link, true);
	eop_link_clear(&link);
	zassert_false(eop_link_wait_up(&link, SHORT_WAIT));
}

ZTEST(link, test_starting_down_then_up_is_seen_by_a_later_wait)
{
	struct eop_link link;

	/* The change happened before anyone waited: the next wait still returns early. */
	eop_link_init(&link, false);
	eop_link_set(&link, true);
	zassert_true(eop_link_wait_up(&link, SHORT_WAIT));
}
