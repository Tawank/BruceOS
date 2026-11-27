#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* Device state is initialized lazily by the public getters. */

/* Drives BRUCE_POWER_HOLD_GPIO high, if configured (-1 = no-op), then
 * brings up the board's HAL devices (src/hal). Call once, as early as
 * possible in app_main() - before storage/display init - on boards that
 * need a GPIO held high to latch their power circuit on or enable the
 * battery-sense path (see Kconfig help text). */
void device__power_hold_init(void);
/* The keyboard backlight at its setting (on) or off, without saving. */
void device__keyboard_light_apply(bool on);
/* Whether a process called device__prevent_sleep(true): no suspend, but the chip may light-sleep. */
bool device__app_awake(void);
/* Why neither the device nor the chip may sleep now ("call", "audio", "usb"), or NULL. */
const char *device__awake_reason(void);
/* Why the chip must not light-sleep even with the screen off ("gps", ...), or NULL. */
const char *device__chip_reason(void);
/* Screen dimming and the sleep timeout; the input process calls it every poll. */
void device__idle_poll(void);

/* Power management (device_pm.c). With FREERTOS_USE_TICKLESS_IDLE the chip
 * light-sleeps whenever every task waits, unless Core holds it awake. */
typedef enum {
    DEVICE__WAITER_INPUT,
    DEVICE__WAITER_BUS,
    DEVICE__WAITERS,
} device__waiter_t;

/* Before hal__init(): sets up waking on HAL interrupt pins and light sleep. */
void device__pm_init(void);
/* Holds the chip awake (no light sleep) or lets it sleep. */
void device__pm_hold(bool awake);
bool device__pm_awake(void);
/* The screen-off sleep that device__sleep() enters; timers keep running. */
bruce_result_t device__pm_sleep(void);
bool device__pm_asleep(void);
void device__pm_wake(bool by_select);
bool device__pm_select_pressed(void);
/* A poll loop's wait: busy_ms while busy; otherwise, where the chip can
 * sleep, up to a second or until a HAL interrupt. BRUCE_ERR_CANCELLED once
 * the process is signalled. */
bruce_result_t device__wait(device__waiter_t waiter, bool busy, uint32_t busy_ms);
/* Ends a wait early (another process has input for it). */
void device__wake_waiter(device__waiter_t waiter);

/* The latest battery level from an I2C gauge, set by device_bus; what
 * device__get_battery() returns on those boards. */
void device__set_battery_internal(int percent, bool charging);
