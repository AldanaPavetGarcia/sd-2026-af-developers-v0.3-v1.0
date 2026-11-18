/*
 * EOP edge firmware (US-401, ADR-014): reads the configuration, sets the wall clock and starts
 * the sensor and MQTT threads. Everything the threads share lives here and is handed to them
 * through their first argument.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "eop_platform.h"
#include "eop_queue.h"
#include "eop_status.h"
#include "eop_threads.h"

LOG_MODULE_REGISTER(eop_main, LOG_LEVEL_INF);

#define QUEUE_ALIGN 4

static struct eop_device_config device_config;
static struct eop_queue telemetry_queue;
static struct eop_status_board status_board;
static struct eop_context context;
static char
	__aligned(QUEUE_ALIGN) queue_buffer[CONFIG_EOP_QUEUE_LEN * sizeof(struct eop_telemetry)];

K_THREAD_STACK_DEFINE(sensor_stack, CONFIG_EOP_SENSOR_STACK_SIZE);
K_THREAD_STACK_DEFINE(mqtt_stack, CONFIG_EOP_MQTT_STACK_SIZE);
static struct k_thread sensor_thread;
static struct k_thread mqtt_thread;

int main(void)
{
	int ret = eop_platform_config(&device_config);

	if (ret < 0) {
		LOG_ERR("invalid configuration; not starting");
		return ret;
	}
	ret = eop_platform_set_clock();
	if (ret < 0) {
		LOG_ERR("wall clock not set (%d); timestamps would be wrong, not starting", ret);
		return ret;
	}

	eop_queue_init(&telemetry_queue, queue_buffer, CONFIG_EOP_QUEUE_LEN);
	eop_status_init(&status_board, &telemetry_queue);
	eop_shell_attach(&status_board);
	context = (struct eop_context){
		.config = &device_config,
		.queue = &telemetry_queue,
		.status = &status_board,
	};

	LOG_INF("node %s (%s), boot %s, broker %s:%u", device_config.node_id,
		device_config.bunker_name, device_config.boot_id, device_config.broker_host,
		device_config.broker_port);

	k_thread_create(&sensor_thread, sensor_stack, K_THREAD_STACK_SIZEOF(sensor_stack),
			eop_sensor_thread, &context, NULL, NULL, CONFIG_EOP_SENSOR_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&sensor_thread, "eop_sensor");
	k_thread_create(&mqtt_thread, mqtt_stack, K_THREAD_STACK_SIZEOF(mqtt_stack),
			eop_mqtt_thread, &context, NULL, NULL, CONFIG_EOP_MQTT_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&mqtt_thread, "eop_mqtt");
	return 0;
}
