#pragma once

#include "hal.h"

/* A power supply switched by a pin, held on while any device using it is
 * powered (Linux's regulators, reduced to on and off). */
typedef struct {
    hal_pin_t enable;     /* Active = on. */
    uint16_t on_delay_ms; /* To settle after switching on. */
} hal_rail_config_t;

extern const hal_driver_t hal_rail_driver;

/* For hal_device_power(): counts a user in or out, switching on the first and off the last. */
bruce_result_t hal_rail__use(const hal_device_t *rail, bool on);

#define HAL_RAIL(id, ...)                                                                         \
    static const hal_rail_config_t id##__rail = {__VA_ARGS__};                                    \
    HAL_DEVICE(id, .driver = &hal_rail_driver, .config = &id##__rail)
