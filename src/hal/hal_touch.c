#include "hal_touch.h"

bruce_result_t hal_touch_read(const hal_device_t *device, hal_touch_report_t *out) {
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out = (hal_touch_report_t){.key = -1};
    if (!hal_device_ready(device) || device->driver->class_id != HAL_CLASS_TOUCH) return BRUCE_ERR_NOT_FOUND;
    const hal_touch_api_t *api = device->driver->api;
    return api->read(device, out);
}

void hal_touch_orient(const hal_touch_orientation_t *orientation, int32_t *x, int32_t *y) {
    if (orientation == NULL) return;
    if (orientation->swap_xy) {
        int32_t swapped = *x;
        *x = *y;
        *y = swapped;
    }
    if (orientation->mirror_x) *x = orientation->width - 1 - *x;
    if (orientation->mirror_y) *y = orientation->height - 1 - *y;
}
