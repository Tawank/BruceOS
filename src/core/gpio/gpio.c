#include "gpio.h"

#include "core_sdk/permission.h"

#include "driver/gpio.h"
#include "esp_err.h"

static bruce_result_t gpio__esp_result(esp_err_t error)
{
    if (error == ESP_OK) return BRUCE_OK;
    if (error == ESP_ERR_INVALID_ARG) return BRUCE_ERR_INVALID_ARGUMENT;
    if (error == ESP_ERR_NOT_SUPPORTED) return BRUCE_ERR_UNSUPPORTED;
    return BRUCE_ERR_IO;
}

bruce_result_t gpio__configure_internal(int pin, bruce_gpio_mode_t mode, bruce_gpio_pull_t pull)
{
    if (!GPIO_IS_VALID_GPIO(pin) || mode < BRUCE_GPIO_MODE_INPUT || mode > BRUCE_GPIO_MODE_INPUT_OUTPUT ||
        pull < BRUCE_GPIO_PULL_NONE || pull > BRUCE_GPIO_PULL_DOWN) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (mode != BRUCE_GPIO_MODE_INPUT && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) return BRUCE_ERR_UNSUPPORTED;

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode == BRUCE_GPIO_MODE_INPUT ? GPIO_MODE_INPUT :
                mode == BRUCE_GPIO_MODE_OUTPUT ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = pull == BRUCE_GPIO_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull == BRUCE_GPIO_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio__esp_result(gpio_config(&config));
}

bruce_result_t gpio__read_internal(int pin, int *out_level)
{
    if (!GPIO_IS_VALID_GPIO(pin) || out_level == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_level = gpio_get_level((gpio_num_t)pin) != 0;
    return BRUCE_OK;
}

bruce_result_t gpio__write_internal(int pin, int level)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || (level != 0 && level != 1)) return BRUCE_ERR_INVALID_ARGUMENT;
    return gpio__esp_result(gpio_set_level((gpio_num_t)pin, level));
}

bruce_result_t gpio__configure(int pin, bruce_gpio_mode_t mode, bruce_gpio_pull_t pull)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK ? gpio__configure_internal(pin, mode, pull) : permission;
}

bruce_result_t gpio__read(int pin, int *out_level)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK ? gpio__read_internal(pin, out_level) : permission;
}

bruce_result_t gpio__write(int pin, int level)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK ? gpio__write_internal(pin, level) : permission;
}
