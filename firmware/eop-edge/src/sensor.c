#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "eop_platform.h"
#include "eop_threads.h"

LOG_MODULE_REGISTER(eop_sensor, LOG_LEVEL_INF);

/* Simulated sensor: a bounded random walk, so the status moves through OK, WARN and ALERT. */
static int16_t next_temperature(int16_t current)
{
	int32_t span = 2 * CONFIG_EOP_TEMP_STEP_DC + 1;
	int32_t next =
		current + (int32_t)(sys_rand32_get() % (uint32_t)span) - CONFIG_EOP_TEMP_STEP_DC;

	return (int16_t)CLAMP(next, CONFIG_EOP_TEMP_MIN_DC, CONFIG_EOP_TEMP_MAX_DC);
}

void eop_sensor_thread(void *ctx_ptr, void *unused2, void *unused3)
{
	struct eop_context *ctx = ctx_ptr;
	struct eop_telemetry sample = {
		.message_id = 0,
		.temperature_dc = CONFIG_EOP_TEMP_INITIAL_DC,
	};
	int64_t next_tick = k_uptime_get();

	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		sample.message_id++;
		sample.sampled_at_ms = eop_platform_now_ms();
		sample.temperature_dc = next_temperature(sample.temperature_dc);
		sample.status = eop_status_for(sample.temperature_dc, CONFIG_EOP_TEMP_WARN_DC,
					       CONFIG_EOP_TEMP_ALERT_DC);
		if (eop_queue_put(ctx->queue, &sample)) {
			LOG_WRN("buffer full: oldest sample discarded (%u since boot)",
				eop_queue_dropped(ctx->queue));
		}

		/* Absolute deadlines keep the period at 1 s even if a put took longer. */
		next_tick += CONFIG_EOP_SAMPLE_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next_tick));
	}
}
