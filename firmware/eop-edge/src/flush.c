#include "eop_flush.h"

bool eop_flush_start(struct eop_flush *flush, bool backlog, uint32_t last_id)
{
	flush->active = backlog;
	flush->last_id = last_id;
	flush->delivered = 0;
	return !backlog;
}

bool eop_flush_delivered(struct eop_flush *flush, uint32_t message_id)
{
	if (!flush->active) {
		return false;
	}
	if (message_id <= flush->last_id) {
		flush->delivered++;
	}
	/* A newer sample means the last buffered one was dropped while flushing: the flush is over
	 * all the same.
	 */
	if (message_id >= flush->last_id) {
		flush->active = false;
		return true;
	}
	return false;
}
