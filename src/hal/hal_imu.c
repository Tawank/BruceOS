#include "hal_imu.h"

bruce_result_t hal_imu_read(const hal_device_t *device, hal_imu_reading_t *out) {
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out = (hal_imu_reading_t){0};
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_IMU) return BRUCE_ERR_NOT_FOUND;
    const hal_imu_api_t *api = device->driver->api;
    return api->read(device, out);
}
