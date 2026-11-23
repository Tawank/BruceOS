#pragma once

#include "hal.h"

typedef struct {
    int percent;   /* 0-100. */
    bool charging; /* On external power: charging or full. */
} hal_battery_reading_t;

/* Operations of a HAL_CLASS_BATTERY driver. */
typedef struct {
    bruce_result_t (*read)(const hal_device_t *device, hal_battery_reading_t *out);
} hal_battery_api_t;

/* Reads a ready battery device. */
bruce_result_t hal_battery_read(const hal_device_t *device, hal_battery_reading_t *out);
