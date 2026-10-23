#include "eop_queue.h"

#define QUEUE_ALIGN 4

void eop_queue_init(struct eop_queue *queue, char *buffer, uint32_t max_samples)
{
	k_msgq_init(&queue->msgq, buffer, sizeof(struct eop_telemetry), max_samples);
	atomic_set(&queue->dropped, 0);
}

bool eop_queue_put(struct eop_queue *queue, const struct eop_telemetry *sample)
{
	struct eop_telemetry oldest;
	bool dropped = false;

	/* The sensor thread is the only producer, so after taking one sample out there is room;
	 * the loop only repeats if the consumer raced us and the put still found it full.
	 */
	while (k_msgq_put(&queue->msgq, sample, K_NO_WAIT) != 0) {
		if (k_msgq_get(&queue->msgq, &oldest, K_NO_WAIT) == 0) {
			atomic_inc(&queue->dropped);
			dropped = true;
		}
	}
	return dropped;
}

int eop_queue_get(struct eop_queue *queue, struct eop_telemetry *sample, k_timeout_t timeout)
{
	return k_msgq_get(&queue->msgq, sample, timeout) == 0 ? 0 : -EAGAIN;
}

uint32_t eop_queue_count(struct eop_queue *queue)
{
	return k_msgq_num_used_get(&queue->msgq);
}

uint32_t eop_queue_dropped(struct eop_queue *queue)
{
	return (uint32_t)atomic_get(&queue->dropped);
}

BUILD_ASSERT(sizeof(struct eop_telemetry) % QUEUE_ALIGN == 0,
	     "k_msgq entries must keep the queue buffer aligned");
