#include "hal_haptic.h"

bruce_result_t hal_haptic_play(const hal_device_t *device, hal_haptic_effect_t effect) {
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_HAPTIC) return BRUCE_ERR_NOT_FOUND;
    const hal_haptic_api_t *api = device->driver->api;
    return api->play(device, effect);
}
