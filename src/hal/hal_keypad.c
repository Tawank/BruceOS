#include "hal_keypad.h"

bruce_result_t hal_keypad_next_event(const hal_device_t *device, hal_keypad_event_t *out) {
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_KEYPAD) return BRUCE_ERR_NOT_FOUND;
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    const hal_keypad_api_t *api = device->driver->api;
    return api->next_event(device, out);
}
