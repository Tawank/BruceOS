#pragma once

#include "driver/i2c_master.h" // IWYU pragma: keep

/* Lazily creates and returns the shared "board I2C" bus (BRUCE_BOARD_I2C_*
 * in Kconfig), used internally by HAL drivers that talk to a PMIC, a
 * backlight GPIO expander, or a touch controller - never by app code. This
 * is deliberately separate from core/i2c/i2c.c's permission-gated, app-facing
 * I2C API: the bus here is owned directly by these drivers, the same way
 * display_driver.c owns its own SPI bus.
 *
 * Returns NULL if BRUCE_BOARD_I2C_ENABLED is not set, or if bus creation
 * failed. Safe to call from multiple drivers/tasks; the bus is created once
 * and the handle is cached for the life of the process.
 */
i2c_master_bus_handle_t board_i2c__acquire(void);
