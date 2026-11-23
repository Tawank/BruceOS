#pragma once

#include "driver/i2c_master.h" // IWYU pragma: keep
#include "hal.h"

/* Operations of a HAL_CLASS_I2C bus driver. */
typedef struct {
    bruce_result_t (*add_device)(const hal_device_t *bus, uint16_t address, uint32_t speed_hz,
                                 i2c_master_dev_handle_t *out);
} hal_i2c_api_t;

/* A handle for `device` on its parent I2C bus, at its address. */
bruce_result_t hal_i2c_open(const hal_device_t *device, uint32_t speed_hz, i2c_master_dev_handle_t *out);

/* One of the ESP32's I2C controllers. */
typedef struct {
    int8_t port; /* I2C_NUM_0, I2C_NUM_1... */
    int8_t sda;
    int8_t scl;
    bool pullup; /* The ESP's internal pull-ups, for boards without their own. */
} hal_i2c_master_config_t;

extern const hal_driver_t hal_i2c_master_driver;

/* The ESP-IDF bus of a ready hal_i2c_master_driver device, else NULL. */
i2c_master_bus_handle_t hal_i2c_bus(const hal_device_t *bus);
