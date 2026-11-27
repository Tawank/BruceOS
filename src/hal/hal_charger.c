#include "hal_charger.h"

bruce_result_t hal_charger_read(const hal_device_t *device, bool measure, bruce_device_power_t *out) {
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_CHARGER) return BRUCE_ERR_NOT_FOUND;
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    const hal_charger_api_t *api = device->driver->api;
    return api->read(device, measure, out);
}

bruce_result_t hal_charger_set_otg(const hal_device_t *device, bool on) {
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_CHARGER) return BRUCE_ERR_NOT_FOUND;
    const hal_charger_api_t *api = device->driver->api;
    return api->set_otg != NULL ? api->set_otg(device, on) : BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t hal_charger_ship_mode(const hal_device_t *device) {
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_CHARGER) return BRUCE_ERR_NOT_FOUND;
    const hal_charger_api_t *api = device->driver->api;
    return api->ship_mode != NULL ? api->ship_mode(device) : BRUCE_ERR_UNSUPPORTED;
}
