#pragma once

#include "hal.h"

/* Operations of a HAL_CLASS_LED driver. */
typedef struct {
    bruce_result_t (*set)(const hal_device_t *device, uint8_t brightness); /* 0 off, 255 full. */
} hal_led_api_t;

bruce_result_t hal_led_set(const hal_device_t *device, uint8_t brightness);
