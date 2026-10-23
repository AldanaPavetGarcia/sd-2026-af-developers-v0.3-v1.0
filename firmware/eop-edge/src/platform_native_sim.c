/*
 * native_sim side of the platform (ADR-014 "Running without an ESP32"): the wall clock comes
 * from the host, and command-line options override the Kconfig identity and broker so one image
 * can run several simulated devices (docker-compose, Kubernetes).
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/clock.h>

#include "cmdline.h"
#include "posix_native_task.h"
#include "native_rtc.h"
#include "platform_board.h"

#define US_PER_SECOND 1000000ULL
#define NS_PER_US     1000ULL
#define UNSET_PORT    UINT32_MAX
#define MAX_PORT      65535U
#define BITS_PER_WORD 32

/* Filled by the native_sim command-line parser before the kernel starts. */
static const char *opt_node_id;
static const char *opt_bunker_name;
static const char *opt_broker_host;
static uint32_t opt_broker_port = UNSET_PORT;

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "node-id",
		 .name = "id",
		 .type = 's',
		 .dest = (void *)&opt_node_id,
		 .descript = "Node id (overrides CONFIG_EOP_NODE_ID)"},
		{.option = "bunker-name",
		 .name = "name",
		 .type = 's',
		 .dest = (void *)&opt_bunker_name,
		 .descript = "Bunker name (overrides CONFIG_EOP_BUNKER_NAME)"},
		{.option = "broker-host",
		 .name = "host",
		 .type = 's',
		 .dest = (void *)&opt_broker_host,
		 .descript = "MQTT broker host (overrides CONFIG_EOP_MQTT_BROKER_HOST)"},
		{.option = "broker-port",
		 .name = "port",
		 .type = 'u',
		 .dest = (void *)&opt_broker_port,
		 .descript = "MQTT broker port (overrides CONFIG_EOP_MQTT_BROKER_PORT)"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}
NATIVE_TASK(add_options, PRE_BOOT_1, 10);

static void override(char *dest, size_t size, const char *value)
{
	if (value != NULL) {
		strncpy(dest, value, size - 1);
		dest[size - 1] = '\0';
	}
}

void eop_board_apply_overrides(struct eop_device_config *config)
{
	override(config->node_id, sizeof(config->node_id), opt_node_id);
	override(config->bunker_name, sizeof(config->bunker_name), opt_bunker_name);
	override(config->broker_host, sizeof(config->broker_host), opt_broker_host);
	if (opt_broker_port != UNSET_PORT) {
		/* Out of range becomes 0, which eop_platform_config() rejects. */
		config->broker_port = opt_broker_port <= MAX_PORT ? (uint16_t)opt_broker_port : 0;
	}
}

uint32_t eop_board_boot_entropy(void)
{
	/* native_sim's entropy driver starts from a fixed seed, so every run would draw the same
	 * boot id; the host clock makes it differ between runs.
	 */
	uint64_t now_us = native_rtc_gettime_us(RTC_CLOCK_PSEUDOHOSTREALTIME);

	return (uint32_t)now_us ^ (uint32_t)(now_us >> BITS_PER_WORD) ^ sys_rand32_get();
}

int eop_board_set_clock(void)
{
	uint64_t now_us = native_rtc_gettime_us(RTC_CLOCK_PSEUDOHOSTREALTIME);
	struct timespec ts = {
		.tv_sec = (time_t)(now_us / US_PER_SECOND),
		.tv_nsec = (long)((now_us % US_PER_SECOND) * NS_PER_US),
	};

	return sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
}
