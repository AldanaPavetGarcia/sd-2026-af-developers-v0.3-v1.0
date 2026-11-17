/*
 * Supervisor thread (US-403 AC1-AC3, ADR-014): every CONFIG_EOP_WDT_CHECK_MS it restarts the
 * threads that did not check in for CONFIG_EOP_WDT_TIMEOUT_MS, one by one, so a hung thread never
 * stops the other. It runs at the highest application priority so a busy thread cannot starve it,
 * and it also picks the LED pattern.
 *
 * A restarted thread keeps its state (message_id, pending sample, buffer): it lives in its module,
 * not on the aborted stack. Threads never block while holding a lock, so the only way to abort one
 * holding a lock is for it to hang inside the critical section, which the watchdog cannot fix.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "eop_threads.h"

LOG_MODULE_REGISTER(eop_wdt, LOG_LEVEL_INF);

void eop_thread_start(struct eop_context *ctx, enum eop_thread_id id)
{
	const struct eop_thread_slot *slot = &ctx->slots[id];

	k_thread_create(slot->thread, slot->stack, slot->stack_size, slot->entry, ctx, NULL, NULL,
			slot->priority, 0, K_NO_WAIT);
	k_thread_name_set(slot->thread, slot->name);
}

void eop_thread_alive(struct eop_context *ctx, enum eop_thread_id id)
{
	eop_wdt_checkin(ctx->wdt, id, k_uptime_get());
}

void eop_fault_point(struct eop_context *ctx, enum eop_thread_id id)
{
	eop_thread_alive(ctx, id);
	if (IS_ENABLED(CONFIG_EOP_FAULT_INJECT) && eop_wdt_take_hang(ctx->wdt, id)) {
		LOG_WRN("fault injection: %s thread hangs", eop_thread_name(id));
		k_sleep(K_FOREVER);
	}
}

static void restart(struct eop_context *ctx, enum eop_thread_id id, int64_t now)
{
	LOG_ERR("%s thread missed its check-in for more than %u ms; restarting it",
		eop_thread_name(id), ctx->wdt->timeout_ms);
	k_thread_abort(ctx->slots[id].thread);
	eop_wdt_restarted(ctx->wdt, id, now);
	eop_thread_start(ctx, id);
	LOG_WRN("%s thread restarted (%u restarts since boot)", eop_thread_name(id),
		eop_wdt_restarts(ctx->wdt, id));
}

void eop_supervisor_thread(void *ctx_ptr, void *unused2, void *unused3)
{
	struct eop_context *ctx = ctx_ptr;
	struct k_timer check;
	struct eop_status_snapshot snap;

	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	k_timer_init(&check, NULL, NULL);
	k_timer_start(&check, K_MSEC(CONFIG_EOP_WDT_CHECK_MS), K_MSEC(CONFIG_EOP_WDT_CHECK_MS));
	for (;;) {
		(void)k_timer_status_sync(&check);
		int64_t now = k_uptime_get();
		uint32_t overdue = eop_wdt_overdue(ctx->wdt, now);

		for (int id = 0; id < EOP_THREAD_COUNT; id++) {
			if ((overdue & BIT(id)) != 0) {
				restart(ctx, (enum eop_thread_id)id, now);
			}
		}

		eop_status_snapshot(ctx->status, &snap);
		eop_led_show(ctx->led, eop_led_pattern_for(snap.conn_state == EOP_CONN_CONNECTED,
							   eop_wdt_recent_restart(
								   ctx->wdt, now,
								   CONFIG_EOP_LED_ERROR_HOLD_MS)));
	}
}
