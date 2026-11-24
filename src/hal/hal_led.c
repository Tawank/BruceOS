#include "hal_led.h"

bruce_result_t hal_led_set(const hal_device_t *device, uint8_t brightness) {
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_LED) return BRUCE_ERR_NOT_FOUND;
    const hal_led_api_t *api = device->driver->api;
    return api->set(device, brightness);
}
