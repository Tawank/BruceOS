#include "hal_battery.h"

bruce_result_t hal_battery_read(const hal_device_t *device, hal_battery_reading_t *out) {
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_BATTERY) return BRUCE_ERR_NOT_FOUND;
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    const hal_battery_api_t *api = device->driver->api;
    return api->read(device, out);
}
