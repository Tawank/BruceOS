#pragma once

#include <stdbool.h>

/* Device state is initialized lazily by the public getters. */

/* Drives BRUCE_POWER_HOLD_GPIO high, if configured (-1 = no-op), then
 * brings up the board's HAL devices (src/hal). Call once, as early as
 * possible in app_main() - before storage/display init - on boards that
 * need a GPIO held high to latch their power circuit on or enable the
 * battery-sense path (see Kconfig help text). */
void device__power_hold_init(void);
/* The keyboard backlight at its setting (on) or off, without saving. */
void device__keyboard_light_apply(bool on);
/* Why the device must not sleep now ("app", "call", "audio", "usb"), or NULL. */
const char *device__awake_reason(void);
/* Screen dimming and the sleep timeout; the input process calls it every poll. */
void device__idle_poll(void);

/* The latest battery level from an I2C gauge, set by device_bus; what
 * device__get_battery() returns on those boards. */
void device__set_battery_internal(int percent, bool charging);
