#include "core/device/device.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "hal/hal_led.h"

/* The saved level, kept here so sleep can restore it for an app without the config permission. */
static int s_percent = -1;

static const hal_device_t *device__keyboard_light(void) { return hal_device_find("keyboard_light"); }

bool device__has_keyboard_light(void) { return device__keyboard_light() != NULL; }

void device__keyboard_light_apply(bool on) {
    const hal_device_t *light = device__keyboard_light();
    if (light == NULL) return;
    if (s_percent < 0) s_percent = config__get_keyboard_light();
    (void)hal_led_set(light, on ? (uint8_t)(s_percent * 255 / 100) : 0);
}

bruce_result_t device__set_keyboard_light(int percent) {
    if (!device__has_keyboard_light()) return BRUCE_ERR_UNSUPPORTED;
    if (percent < 0 || percent > 100) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = config__set_keyboard_light(percent);
    if (result != BRUCE_OK) return result;
    s_percent = percent;
    device__keyboard_light_apply(true);
    return BRUCE_OK;
}
