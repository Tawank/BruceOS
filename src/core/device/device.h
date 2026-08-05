#pragma once

/* Device state is initialized lazily by the public getters. */

/* Drives BRUCE_POWER_HOLD_GPIO high, if configured (-1 = no-op). Call once,
 * as early as possible in app_main() - before storage/display init - on
 * boards that need a GPIO held high to latch their power circuit on or
 * enable the battery-sense path (see Kconfig help text). */
void device__power_hold_init(void);
