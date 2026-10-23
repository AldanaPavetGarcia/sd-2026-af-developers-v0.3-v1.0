#include "eop_platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>

#include "platform_board.h"

LOG_MODULE_REGISTER(eop_platform, LOG_LEVEL_INF);

#define MS_PER_SECOND 1000
#define NS_PER_MS     1000000

static void copy_string(char *dest, size_t size, const char *src)
{
	strncpy(dest, src, size - 1);
	dest[size - 1] = '\0';
}

int eop_platform_config(struct eop_device_config *config)
{
	copy_string(config->node_id, sizeof(config->node_id), CONFIG_EOP_NODE_ID);
	copy_string(config->bunker_name, sizeof(config->bunker_name), CONFIG_EOP_BUNKER_NAME);
	copy_string(config->broker_host, sizeof(config->broker_host), CONFIG_EOP_MQTT_BROKER_HOST);
	config->broker_port = CONFIG_EOP_MQTT_BROKER_PORT;
	eop_board_apply_overrides(config);

	(void)snprintf(config->boot_id, sizeof(config->boot_id), "%08x",
		       (unsigned int)eop_board_boot_entropy());

	if (!eop_node_id_valid(config->node_id)) {
		LOG_ERR("invalid node id '%s': expected [a-z0-9-]{1,63}, not starting with %s",
			config->node_id, EOP_RESERVED_NODE_PREFIX);
		return -EINVAL;
	}
	if (!eop_bunker_name_valid(config->bunker_name)) {
		LOG_ERR("invalid bunker name '%s': expected [A-Za-z0-9_.-]{1,63}",
			config->bunker_name);
		return -EINVAL;
	}
	if (config->broker_host[0] == '\0' || config->broker_port == 0) {
		LOG_ERR("broker host and port must be set");
		return -EINVAL;
	}
	return 0;
}

int eop_platform_set_clock(void)
{
	return eop_board_set_clock();
}

int64_t eop_platform_now_ms(void)
{
	struct timespec ts;

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
		return -1;
	}
	return (int64_t)ts.tv_sec * MS_PER_SECOND + ts.tv_nsec / NS_PER_MS;
}
