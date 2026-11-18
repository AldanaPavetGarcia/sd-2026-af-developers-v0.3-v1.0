/**
 * @file eop_platform.h
 * @brief What differs between the ESP32 and native_sim builds (ADR-014).
 * @details Two things depend on the board: where the wall clock comes from (SNTP on the ESP32,
 *          the host on native_sim) and whether the configuration can be overridden at start
 *          (native_sim command-line options, so several simulated devices share one image).
 */

#ifndef EOP_PLATFORM_H
#define EOP_PLATFORM_H

#include <stdint.h>

#include "eop_telemetry.h"

/**
 * @brief Fills @p config from Kconfig, applies the board's overrides and draws the boot id.
 * @param config Configuration to fill.
 * @return 0, or -EINVAL if the resulting node id, bunker name or broker port is invalid.
 */
int eop_platform_config(struct eop_device_config *config);

/**
 * @brief Sets the wall clock (SYS_CLOCK_REALTIME) used for telemetry timestamps.
 * @return 0, or a negative errno if the clock could not be set (timestamps would be wrong).
 */
int eop_platform_set_clock(void);

/**
 * @brief Current wall clock in milliseconds since the Unix epoch.
 * @return Milliseconds, or -1 if the clock is unavailable.
 */
int64_t eop_platform_now_ms(void);

#endif /* EOP_PLATFORM_H */
