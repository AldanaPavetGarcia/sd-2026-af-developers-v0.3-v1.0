#include <zephyr/ztest.h>

#include "eop_queue.h"

#define CAPACITY 60

static char __aligned(4) storage[CAPACITY * sizeof(struct eop_telemetry)];
static struct eop_queue queue;

static void reset(void *fixture)
{
	ARG_UNUSED(fixture);
	eop_queue_init(&queue, storage, CAPACITY);
}

ZTEST_SUITE(queue, NULL, NULL, reset, NULL, NULL);

static struct eop_telemetry sample(uint32_t id)
{
	return (struct eop_telemetry){.message_id = id, .sampled_at_ms = id};
}

ZTEST(queue, test_fifo_order)
{
	struct eop_telemetry out;

	for (uint32_t id = 1; id <= 5; id++) {
		struct eop_telemetry in = sample(id);

		zassert_false(eop_queue_put(&queue, &in));
	}
	zassert_equal(eop_queue_count(&queue), 5);
	for (uint32_t id = 1; id <= 5; id++) {
		zassert_ok(eop_queue_get(&queue, &out, K_NO_WAIT));
		zassert_equal(out.message_id, id);
	}
	zassert_equal(eop_queue_get(&queue, &out, K_NO_WAIT), -EAGAIN);
}

/* A full queue keeps the newest 60 samples and counts what it discarded (US-402 AC1, AC3). */
ZTEST(queue, test_full_queue_discards_the_oldest)
{
	struct eop_telemetry out;

	for (uint32_t id = 1; id <= CAPACITY; id++) {
		struct eop_telemetry in = sample(id);

		zassert_false(eop_queue_put(&queue, &in), "sample %u", id);
	}
	for (uint32_t id = CAPACITY + 1; id <= CAPACITY + 3; id++) {
		struct eop_telemetry in = sample(id);

		zassert_true(eop_queue_put(&queue, &in), "sample %u", id);
	}
	zassert_equal(eop_queue_count(&queue), CAPACITY);
	zassert_equal(eop_queue_dropped(&queue), 3);

	zassert_ok(eop_queue_get(&queue, &out, K_NO_WAIT));
	zassert_equal(out.message_id, 4, "oldest kept sample");
	uint32_t last = out.message_id;

	while (eop_queue_get(&queue, &out, K_NO_WAIT) == 0) {
		zassert_equal(out.message_id, last + 1, "FIFO order after discards");
		last = out.message_id;
	}
	zassert_equal(last, CAPACITY + 3);
}

ZTEST(queue, test_get_waits_for_a_sample)
{
	struct eop_telemetry out;
	int64_t start = k_uptime_get();

	zassert_equal(eop_queue_get(&queue, &out, K_MSEC(50)), -EAGAIN);
	zassert_true(k_uptime_get() - start >= 50, "returned before the timeout");
}

/* The newest id marks the end of a flush after reconnecting (US-402 AC6), dropped or not. */
ZTEST(queue, test_remembers_the_newest_id)
{
	struct eop_telemetry out;

	zassert_equal(eop_queue_newest_id(&queue), 0);
	for (uint32_t id = 1; id <= CAPACITY + 5; id++) {
		struct eop_telemetry in = sample(id);

		(void)eop_queue_put(&queue, &in);
		zassert_equal(eop_queue_newest_id(&queue), id);
	}
	while (eop_queue_get(&queue, &out, K_NO_WAIT) == 0) {
	}
	zassert_equal(eop_queue_newest_id(&queue), CAPACITY + 5, "still known once consumed");
}
