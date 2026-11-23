#pragma once

#include "hal.h"

/* Operations of a HAL_CLASS_GPIO driver, on raw (electrical) levels. */
typedef struct {
    bruce_result_t (*set_output)(const hal_device_t *controller, uint8_t number, bool high);
    bruce_result_t (*get)(const hal_device_t *controller, uint8_t number, bool *high);
} hal_gpio_api_t;

/* Makes the pin an output and drives it active or inactive. NULL pin: BRUCE_ERR_NOT_FOUND. */
bruce_result_t hal_pin_set(const hal_pin_t *pin, bool active);
bruce_result_t hal_pin_get(const hal_pin_t *pin, bool *active);
/* The SoC GPIO number, or -1 when the pin is on another controller. */
int hal_pin_soc_gpio(const hal_pin_t *pin);
