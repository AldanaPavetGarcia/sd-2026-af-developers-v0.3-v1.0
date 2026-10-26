#include "eop_status.h"

#include <zephyr/shell/shell.h>

#include "eop_platform.h"

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

/* The shell API passes no user data to a command handler, so what it uses is kept here. */
static struct eop_status_board *shell_board;
static struct eop_link *shell_link;

void eop_shell_attach(struct eop_status_board *board, struct eop_link *link)
{
	shell_board = board;
	shell_link = link;
}

static int cmd_eop_status(const struct shell *sh, size_t argc, char **argv)
{
	struct eop_status_snapshot snap;
	char last[EOP_TIMESTAMP_LEN + 1] = "never";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (shell_board == NULL || shell_link == NULL) {
		shell_error(sh, "status not available yet");
		return -EAGAIN;
	}
	eop_status_snapshot(shell_board, &snap);
	if (snap.last_published_ms > 0) {
		(void)eop_format_timestamp(snap.last_published_ms, last, sizeof(last));
	}
	shell_print(sh, "link:           %s", eop_link_is_up(shell_link) ? "up" : "down");
	shell_print(sh, "connection:     %s", eop_conn_state_name(snap.conn_state));
	shell_print(sh, "last published: %s", last);
	shell_print(sh, "messages sent:  %u", snap.messages_sent);
	shell_print(sh, "buffered:       %u", snap.buffered);
	shell_print(sh, "dropped:        %u (oldest samples discarded when the buffer is full)",
		    snap.dropped);
	shell_print(sh, "connect failures: %u", snap.connect_failures);
	shell_print(sh, "reconnections:  %u (last one delivered %u buffered samples)",
		    snap.reconnections, snap.last_delivered);
	return 0;
}

#if defined(CONFIG_BOARD_NATIVE_SIM)
/* Simulated WiFi loss (ADR-014): the MQTT thread drops the connection and the buffer fills. */
static int set_link(const struct shell *sh, bool up)
{
	if (shell_link == NULL) {
		shell_error(sh, "link not available yet");
		return -EAGAIN;
	}
	if (!eop_link_set(shell_link, up)) {
		shell_print(sh, "link already %s", up ? "up" : "down");
		return 0;
	}
	shell_print(sh, "link %s", up ? "up: reconnecting now" : "down: buffering samples");
	return 0;
}

static int cmd_eop_net_down(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return set_link(sh, false);
}

static int cmd_eop_net_up(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return set_link(sh, true);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	eop_net_cmds, SHELL_CMD(down, NULL, "Simulate losing the network", cmd_eop_net_down),
	SHELL_CMD(up, NULL, "Bring the simulated network back", cmd_eop_net_up),
	SHELL_SUBCMD_SET_END);
#define EOP_NET_CMD SHELL_CMD(net, &eop_net_cmds, "Simulated network link (native_sim)", NULL),
#else
#define EOP_NET_CMD
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(eop_cmds,
			       SHELL_CMD(status, NULL, "Connection state and telemetry counters",
					 cmd_eop_status),
			       EOP_NET_CMD SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(eop, &eop_cmds, "EOP edge device commands", NULL);
