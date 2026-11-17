/*
 * EOP edge firmware (US-401..US-403, ADR-014): reads the configuration, sets the wall clock and
 * starts the sensor and MQTT threads under the supervisor. Everything the threads share lives
 * here and is handed to them through their first argument.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "eop_led.h"
#include "eop_link.h"
#include "eop_platform.h"
#include "eop_queue.h"
#include "eop_status.h"
#include "eop_threads.h"
#include "eop_watchdog.h"

LOG_MODULE_REGISTER(eop_main, LOG_LEVEL_INF);

#define QUEUE_ALIGN 4

static struct eop_device_config device_config;
static struct eop_queue telemetry_queue;
static struct eop_status_board status_board;
static struct eop_link network_link;
static struct eop_watchdog watchdog;
static struct eop_led status_led;
static struct eop_context context;
static char
	__aligned(QUEUE_ALIGN) queue_buffer[CONFIG_EOP_QUEUE_LEN * sizeof(struct eop_telemetry)];

K_THREAD_STACK_DEFINE(sensor_stack, CONFIG_EOP_SENSOR_STACK_SIZE);
K_THREAD_STACK_DEFINE(mqtt_stack, CONFIG_EOP_MQTT_STACK_SIZE);
K_THREAD_STACK_DEFINE(supervisor_stack, CONFIG_EOP_SUPERVISOR_STACK_SIZE);
static struct k_thread sensor_thread;
static struct k_thread mqtt_thread;
static struct k_thread supervisor_thread;

static const struct eop_thread_slot thread_slots[EOP_THREAD_COUNT] = {
	[EOP_THREAD_SENSOR] =
		{
			.thread = &sensor_thread,
			.stack = sensor_stack,
			.stack_size = K_THREAD_STACK_SIZEOF(sensor_stack),
			.entry = eop_sensor_thread,
			.priority = CONFIG_EOP_SENSOR_PRIORITY,
			.name = "eop_sensor",
		},
	[EOP_THREAD_MQTT] =
		{
			.thread = &mqtt_thread,
			.stack = mqtt_stack,
			.stack_size = K_THREAD_STACK_SIZEOF(mqtt_stack),
			.entry = eop_mqtt_thread,
			.priority = CONFIG_EOP_MQTT_PRIORITY,
			.name = "eop_mqtt",
		},
};

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
	eop_link_init(&network_link, true);
	eop_wdt_init(&watchdog, CONFIG_EOP_WDT_TIMEOUT_MS, k_uptime_get());
	(void)eop_led_start(&status_led);
	context = (struct eop_context){
		.config = &device_config,
		.queue = &telemetry_queue,
		.status = &status_board,
		.link = &network_link,
		.wdt = &watchdog,
		.led = &status_led,
		.slots = thread_slots,
	};
	eop_shell_attach(&context);

	LOG_INF("node %s (%s), boot %s, broker %s:%u", device_config.node_id,
		device_config.bunker_name, device_config.boot_id, device_config.broker_host,
		device_config.broker_port);

	eop_thread_start(&context, EOP_THREAD_SENSOR);
	eop_thread_start(&context, EOP_THREAD_MQTT);
	k_thread_create(&supervisor_thread, supervisor_stack,
			K_THREAD_STACK_SIZEOF(supervisor_stack), eop_supervisor_thread, &context,
			NULL, NULL, CONFIG_EOP_SUPERVISOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&supervisor_thread, "eop_supervisor");
	return 0;
}
