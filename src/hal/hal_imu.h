#pragma once

#include "hal.h"

typedef struct {
    bool valid;    /* A reading has arrived since the device started. */
    float yaw_deg; /* Turn about the vertical, counterclockwise seen from above, from an arbitrary start. */
} hal_imu_reading_t;

/* Operations of a HAL_CLASS_IMU driver; readings flow between hal_device_get() and put(). */
typedef struct {
    bruce_result_t (*read)(const hal_device_t *device, hal_imu_reading_t *out);
} hal_imu_api_t;

bruce_result_t hal_imu_read(const hal_device_t *device, hal_imu_reading_t *out);
