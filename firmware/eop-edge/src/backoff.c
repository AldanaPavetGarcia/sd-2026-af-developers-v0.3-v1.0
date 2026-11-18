#include "eop_backoff.h"

#define BACKOFF_FACTOR 2U

void eop_backoff_init(struct eop_backoff *backoff, uint32_t initial_s, uint32_t max_s)
{
	backoff->initial_s = initial_s == 0U ? 1U : initial_s;
	backoff->max_s = max_s < backoff->initial_s ? backoff->initial_s : max_s;
	backoff->next_s = backoff->initial_s;
}

uint32_t eop_backoff_next(struct eop_backoff *backoff)
{
	uint32_t delay = backoff->next_s;

	/* Compare before multiplying so the doubling can never overflow. */
	if (backoff->next_s > backoff->max_s / BACKOFF_FACTOR) {
		backoff->next_s = backoff->max_s;
	} else {
		backoff->next_s *= BACKOFF_FACTOR;
	}
	return delay;
}

void eop_backoff_reset(struct eop_backoff *backoff)
{
	backoff->next_s = backoff->initial_s;
}
