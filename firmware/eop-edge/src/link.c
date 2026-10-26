#include "eop_link.h"

#define CAME_UP_LIMIT 1

void eop_link_init(struct eop_link *link, bool up)
{
	atomic_set(&link->up, up ? 1 : 0);
	k_sem_init(&link->came_up, 0, CAME_UP_LIMIT);
}

bool eop_link_set(struct eop_link *link, bool up)
{
	bool was_up = atomic_set(&link->up, up ? 1 : 0) != 0;

	if (up && !was_up) {
		k_sem_give(&link->came_up);
	}
	return up != was_up;
}

bool eop_link_is_up(struct eop_link *link)
{
	return atomic_get(&link->up) != 0;
}

bool eop_link_wait_up(struct eop_link *link, k_timeout_t timeout)
{
	return k_sem_take(&link->came_up, timeout) == 0;
}

void eop_link_clear(struct eop_link *link)
{
	k_sem_reset(&link->came_up);
}
