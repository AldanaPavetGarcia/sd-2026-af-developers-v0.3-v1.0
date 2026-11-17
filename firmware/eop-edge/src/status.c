#include "eop_status.h"

static const char *const CONN_STATE_NAMES[] = {
	[EOP_CONN_DISCONNECTED] = "disconnected",
	[EOP_CONN_CONNECTING] = "connecting",
	[EOP_CONN_CONNECTED] = "connected",
};

#define NO_PUBACK_YET (-1)

void eop_status_init(struct eop_status_board *board, struct eop_queue *queue)
{
	k_mutex_init(&board->lock);
	board->conn_state = EOP_CONN_DISCONNECTED;
	board->last_published_ms = 0;
	board->messages_sent = 0;
	board->connect_failures = 0;
	board->first_puback_uptime_ms = NO_PUBACK_YET;
	board->reconnections = 0;
	board->last_delivered = 0;
	board->queue = queue;
}

void eop_status_set_conn(struct eop_status_board *board, enum eop_conn_state state)
{
	k_mutex_lock(&board->lock, K_FOREVER);
	board->conn_state = state;
	k_mutex_unlock(&board->lock);
}

void eop_status_connect_failed(struct eop_status_board *board)
{
	k_mutex_lock(&board->lock, K_FOREVER);
	board->connect_failures++;
	k_mutex_unlock(&board->lock);
}

bool eop_status_published(struct eop_status_board *board, int64_t sampled_at_ms)
{
	bool first;

	k_mutex_lock(&board->lock, K_FOREVER);
	board->last_published_ms = sampled_at_ms;
	board->messages_sent++;
	first = board->first_puback_uptime_ms == NO_PUBACK_YET;
	if (first) {
		board->first_puback_uptime_ms = k_uptime_get();
	}
	k_mutex_unlock(&board->lock);
	return first;
}

void eop_status_reconnected(struct eop_status_board *board, uint32_t delivered)
{
	k_mutex_lock(&board->lock, K_FOREVER);
	board->reconnections++;
	board->last_delivered = delivered;
	k_mutex_unlock(&board->lock);
}

void eop_status_snapshot(struct eop_status_board *board, struct eop_status_snapshot *out)
{
	k_mutex_lock(&board->lock, K_FOREVER);
	out->conn_state = board->conn_state;
	out->last_published_ms = board->last_published_ms;
	out->messages_sent = board->messages_sent;
	out->connect_failures = board->connect_failures;
	out->reconnections = board->reconnections;
	out->last_delivered = board->last_delivered;
	k_mutex_unlock(&board->lock);
	out->buffered = eop_queue_count(board->queue);
	out->dropped = eop_queue_dropped(board->queue);
}

const char *eop_conn_state_name(enum eop_conn_state state)
{
	if ((unsigned int)state >= ARRAY_SIZE(CONN_STATE_NAMES)) {
		return CONN_STATE_NAMES[EOP_CONN_DISCONNECTED];
	}
	return CONN_STATE_NAMES[state];
}
