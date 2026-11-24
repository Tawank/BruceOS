#include "hal_hog.h"

#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "hal_gpio.h"

static bruce_result_t hal_hog__probe(const hal_device_t *device) {
    const hal_hog_config_t *config = device->config;
    bool active = config != NULL && config->active;
    for (size_t i = 0; i < device->pin_count; ++i) {
        const hal_pin_t *pin = &device->pins[i].pin;
        int gpio = hal_pin_soc_gpio(pin);
        if (gpio < 0) {
            bruce_result_t result = hal_pin_set(pin, active);
            if (result != BRUCE_OK) return result;
            continue;
        }
        gpio_config_t output = {.pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_OUTPUT};
        if (gpio_config(&output) != ESP_OK) return BRUCE_ERR_INVALID_ARGUMENT;
        (void)gpio_set_level((gpio_num_t)gpio, active != ((pin->flags & HAL_PIN_ACTIVE_LOW) != 0));
        /* The level holds; hand the pin back so the driver that later takes it claims it without a conflict warning. */
        (void)esp_gpio_revoke(1ULL << gpio);
    }
    return BRUCE_OK;
}

const hal_driver_t hal_hog_driver = {
    .name = "hog",
    .class_id = HAL_CLASS_NONE,
    .probe = hal_hog__probe,
};
