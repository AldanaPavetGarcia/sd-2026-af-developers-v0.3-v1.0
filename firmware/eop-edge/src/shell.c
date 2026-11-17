/*
 * The `eop` shell commands (US-401 AC8, US-402 AC3, US-403): device status, the simulated network
 * link on native_sim and, in test builds, fault injection.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "eop_platform.h"
#include "eop_threads.h"

/* The shell API passes no user data to a command handler, so the context is kept here. */
static struct eop_context *shell_ctx;

void eop_shell_attach(struct eop_context *ctx)
{
	shell_ctx = ctx;
}

static bool ready(const struct shell *sh)
{
	if (shell_ctx == NULL) {
		shell_error(sh, "device not started yet");
		return false;
	}
	return true;
}

static int cmd_eop_status(const struct shell *sh, size_t argc, char **argv)
{
	struct eop_status_snapshot snap;
	char last[EOP_TIMESTAMP_LEN + 1] = "never";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!ready(sh)) {
		return -EAGAIN;
	}
	eop_status_snapshot(shell_ctx->status, &snap);
	if (snap.last_published_ms > 0) {
		(void)eop_format_timestamp(snap.last_published_ms, last, sizeof(last));
	}
	shell_print(sh, "link:           %s", eop_link_is_up(shell_ctx->link) ? "up" : "down");
	shell_print(sh, "connection:     %s", eop_conn_state_name(snap.conn_state));
	shell_print(sh, "last published: %s", last);
	shell_print(sh, "messages sent:  %u", snap.messages_sent);
	shell_print(sh, "buffered:       %u", snap.buffered);
	shell_print(sh, "dropped:        %u (oldest samples discarded when the buffer is full)",
		    snap.dropped);
	shell_print(sh, "connect failures: %u", snap.connect_failures);
	shell_print(sh, "reconnections:  %u (last one delivered %u buffered samples)",
		    snap.reconnections, snap.last_delivered);
	shell_print(sh, "restarts:       sensor %u, mqtt %u",
		    eop_wdt_restarts(shell_ctx->wdt, EOP_THREAD_SENSOR),
		    eop_wdt_restarts(shell_ctx->wdt, EOP_THREAD_MQTT));
	shell_print(sh, "led:            %s",
		    eop_led_pattern_name(eop_led_current(shell_ctx->led)));
	return 0;
}

#if defined(CONFIG_BOARD_NATIVE_SIM)
/* Simulated WiFi loss (ADR-014): the MQTT thread drops the connection and the buffer fills. */
static int set_link(const struct shell *sh, bool up)
{
	if (!ready(sh)) {
		return -EAGAIN;
	}
	if (!eop_link_set(shell_ctx->link, up)) {
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

#if defined(CONFIG_EOP_FAULT_INJECT)
/* Test builds only (US-403 AC6): make a thread stop checking in, or raise a fatal error. */
static int hang(const struct shell *sh, enum eop_thread_id id)
{
	if (!ready(sh)) {
		return -EAGAIN;
	}
	eop_wdt_request_hang(shell_ctx->wdt, id);
	shell_print(sh, "%s thread will hang at its next check-in", eop_thread_name(id));
	return 0;
}

static int cmd_eop_fault_hang_sensor(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return hang(sh, EOP_THREAD_SENSOR);
}

static int cmd_eop_fault_hang_mqtt(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return hang(sh, EOP_THREAD_MQTT);
}

static int cmd_eop_fault_crash(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "raising a fatal kernel error");
	k_oops();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	eop_hang_cmds, SHELL_CMD(sensor, NULL, "Hang the sensor thread", cmd_eop_fault_hang_sensor),
	SHELL_CMD(mqtt, NULL, "Hang the MQTT thread", cmd_eop_fault_hang_mqtt),
	SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(
	eop_fault_cmds, SHELL_CMD(hang, &eop_hang_cmds, "Make a thread stop checking in", NULL),
	SHELL_CMD(crash, NULL, "Raise a fatal kernel error (the device reboots)",
		  cmd_eop_fault_crash),
	SHELL_SUBCMD_SET_END);
#define EOP_FAULT_CMD SHELL_CMD(fault, &eop_fault_cmds, "Fault injection (test builds)", NULL),
#else
#define EOP_FAULT_CMD
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(eop_cmds,
			       SHELL_CMD(status, NULL, "Connection state and telemetry counters",
					 cmd_eop_status),
			       EOP_NET_CMD EOP_FAULT_CMD SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(eop, &eop_cmds, "EOP edge device commands", NULL);
