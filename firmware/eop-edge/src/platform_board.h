/*
 * Board hooks behind eop_platform.h. Exactly one of platform_native_sim.c and platform_sntp.c is
 * built (CMakeLists.txt), and each implements these three functions.
 */

#ifndef EOP_PLATFORM_BOARD_H
#define EOP_PLATFORM_BOARD_H

#include <stdint.h>

#include "eop_telemetry.h"

/* Replaces Kconfig values with whatever the board lets the operator set at start. */
void eop_board_apply_overrides(struct eop_device_config *config);

/* 32 bits that differ from one boot to the next, for the boot id. */
uint32_t eop_board_boot_entropy(void);

/* Sets SYS_CLOCK_REALTIME; 0 or a negative errno. */
int eop_board_set_clock(void);

#endif /* EOP_PLATFORM_BOARD_H */
