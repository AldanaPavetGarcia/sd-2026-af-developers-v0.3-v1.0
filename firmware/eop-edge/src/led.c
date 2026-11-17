/*
 * Status LED driver (US-403 AC5): the pattern of led_pattern.c on the GPIO of the devicetree alias
 * led0. A board without that alias still keeps the pattern, shown by `eop status`.
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "eop_led.h"

LOG_MODULE_REGISTER(eop_led, LOG_LEVEL_INF);

#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led_gpio = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_LED 1
#else
#define HAVE_LED 0
#endif

static void on_tick(struct k_timer *timer)
{
	struct eop_led *led = CONTAINER_OF(timer, struct eop_led, timer);
	uint32_t tick = (uint32_t)atomic_inc(&led->tick);

#if HAVE_LED
	(void)gpio_pin_set_dt(&led_gpio,
			      eop_led_level((enum eop_led_pattern)atomic_get(&led->pattern), tick));
#else
	ARG_UNUSED(tick);
#endif
}

int eop_led_start(struct eop_led *led)
{
	atomic_set(&led->pattern, EOP_LED_OFFLINE);
	atomic_set(&led->tick, 0);
#if HAVE_LED
	if (!gpio_is_ready_dt(&led_gpio)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(&led_gpio, GPIO_OUTPUT_INACTIVE);

	if (ret < 0) {
		LOG_ERR("LED GPIO not configured (%d)", ret);
		return ret;
	}
#else
	LOG_WRN("no led0 alias on this board: the pattern is only shown by `eop status`");
#endif
	k_timer_init(&led->timer, on_tick, NULL);
	k_timer_start(&led->timer, K_MSEC(EOP_LED_TICK_MS), K_MSEC(EOP_LED_TICK_MS));
	return 0;
}

void eop_led_show(struct eop_led *led, enum eop_led_pattern pattern)
{
	if (atomic_set(&led->pattern, pattern) != pattern) {
		atomic_set(&led->tick, 0);
	}
}

enum eop_led_pattern eop_led_current(struct eop_led *led)
{
	return (enum eop_led_pattern)atomic_get(&led->pattern);
}
