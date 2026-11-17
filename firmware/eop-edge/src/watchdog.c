#include "eop_watchdog.h"

#include <zephyr/sys/util.h>

static const char *const THREAD_NAMES[] = {
	[EOP_THREAD_SENSOR] = "sensor",
	[EOP_THREAD_MQTT] = "mqtt",
};

/* Uptime in the 32 bits an atomic_t holds on every target. */
static atomic_val_t low32(int64_t ms)
{
	return (atomic_val_t)(uint32_t)ms;
}

static uint32_t elapsed(int64_t now_ms, atomic_t *since)
{
	return (uint32_t)now_ms - (uint32_t)atomic_get(since);
}

void eop_wdt_init(struct eop_watchdog *wdt, uint32_t timeout_ms, int64_t now_ms)
{
	wdt->timeout_ms = timeout_ms;
	for (int i = 0; i < EOP_THREAD_COUNT; i++) {
		atomic_set(&wdt->entry[i].last_checkin_ms, low32(now_ms));
		atomic_set(&wdt->entry[i].restarts, 0);
		atomic_set(&wdt->entry[i].hang_requested, 0);
	}
	atomic_set(&wdt->last_restart_ms, 0);
	atomic_set(&wdt->any_restart, 0);
}

void eop_wdt_checkin(struct eop_watchdog *wdt, enum eop_thread_id id, int64_t now_ms)
{
	atomic_set(&wdt->entry[id].last_checkin_ms, low32(now_ms));
}

uint32_t eop_wdt_overdue(struct eop_watchdog *wdt, int64_t now_ms)
{
	uint32_t mask = 0;

	for (int i = 0; i < EOP_THREAD_COUNT; i++) {
		if (elapsed(now_ms, &wdt->entry[i].last_checkin_ms) > wdt->timeout_ms) {
			mask |= BIT(i);
		}
	}
	return mask;
}

void eop_wdt_restarted(struct eop_watchdog *wdt, enum eop_thread_id id, int64_t now_ms)
{
	atomic_inc(&wdt->entry[id].restarts);
	atomic_set(&wdt->entry[id].last_checkin_ms, low32(now_ms));
	atomic_set(&wdt->last_restart_ms, low32(now_ms));
	atomic_set(&wdt->any_restart, 1);
}

uint32_t eop_wdt_restarts(struct eop_watchdog *wdt, enum eop_thread_id id)
{
	return (uint32_t)atomic_get(&wdt->entry[id].restarts);
}

bool eop_wdt_recent_restart(struct eop_watchdog *wdt, int64_t now_ms, uint32_t window_ms)
{
	return atomic_get(&wdt->any_restart) != 0 &&
	       elapsed(now_ms, &wdt->last_restart_ms) < window_ms;
}

const char *eop_thread_name(enum eop_thread_id id)
{
	if ((unsigned int)id >= ARRAY_SIZE(THREAD_NAMES)) {
		return "unknown";
	}
	return THREAD_NAMES[id];
}

void eop_wdt_request_hang(struct eop_watchdog *wdt, enum eop_thread_id id)
{
	atomic_set(&wdt->entry[id].hang_requested, 1);
}

bool eop_wdt_take_hang(struct eop_watchdog *wdt, enum eop_thread_id id)
{
	return atomic_cas(&wdt->entry[id].hang_requested, 1, 0);
}
