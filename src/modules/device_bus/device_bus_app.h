#pragma once

/* Entry point for the device_bus process: a normal built-in
 * (registered like any other via app_runner__register_defaults(), visible
 * in process__list()) that owns the shared board I2C bus (see
 * core/device/board_i2c.h) for the life of the firmware, polling the touch
 * controller and any I2C fuel-gauge/PMIC battery backend and publishing
 * their readings on BRUCE_DEVICE_TOPIC_TOUCH / BRUCE_DEVICE_TOPIC_BATTERY
 * (core_sdk/device.h, core_sdk/pubsub.h) instead of letting anything else
 * touch the bus or the controllers directly.
 *
 * The bus itself is not process-scoped: display_driver.c also acquires it
 * once at boot, before any Bruce process (including this one) exists, to
 * enable the backlight LDO on AXP-PMIC boards. board_i2c__acquire()'s
 * cached-handle design lets both consumers share the one physical bus
 * safely, which is why this process does not close it on exit. This is
 * also why this module - unlike every other built-in - includes a
 * Core-private header (core/device/board_i2c.h) instead of being
 * core_sdk-only; see modules/selftest's equivalent exemption in
 * CMakeLists.txt for the precedent.
 *
 * A no-op loop on boards with neither touch nor an I2C battery backend
 * configured. */
int device_bus_app_main(int argc, char **argv);
