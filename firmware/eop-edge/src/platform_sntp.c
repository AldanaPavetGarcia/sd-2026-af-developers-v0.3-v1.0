/*
 * Board side of the platform for real hardware (ESP32): the configuration is the Kconfig one,
 * the boot id comes from the hardware RNG and the wall clock is set with SNTP at boot.
 */

#include <errno.h>
#include <stdint.h>
#include <time.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/clock.h>

#include "platform_board.h"

LOG_MODULE_REGISTER(eop_sntp, LOG_LEVEL_INF);

#define NS_PER_SECOND  1000000000ULL
#define FRACTION_SHIFT 32

void eop_board_apply_overrides(struct eop_device_config *config)
{
	ARG_UNUSED(config);
}

uint32_t eop_board_boot_entropy(void)
{
	return sys_rand32_get();
}

int eop_board_set_clock(void)
{
	struct sntp_time now;
	int ret;

	ret = sntp_simple(CONFIG_EOP_SNTP_SERVER, CONFIG_EOP_SNTP_TIMEOUT_MS, &now);

	if (ret < 0) {
		LOG_ERR("SNTP query to %s failed (%d)", CONFIG_EOP_SNTP_SERVER, ret);
		return ret;
	}

	/* SNTP fractions are 1/2^32 of a second. */
	struct timespec ts = {
		.tv_sec = (time_t)now.seconds,
		.tv_nsec = (long)(((uint64_t)now.fraction * NS_PER_SECOND) >> FRACTION_SHIFT),
	};

	return sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
}
