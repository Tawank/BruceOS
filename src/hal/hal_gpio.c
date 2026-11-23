#include "hal_gpio.h"

#include "driver/gpio.h"

static const hal_gpio_api_t *hal_gpio__api(const hal_pin_t *pin) {
    const hal_device_t *controller = pin->controller;
    if (!hal_device_ready(controller) || controller->driver->class_id != HAL_CLASS_GPIO) return NULL;
    return controller->driver->api;
}

bruce_result_t hal_pin_set(const hal_pin_t *pin, bool active) {
    if (pin == NULL) return BRUCE_ERR_NOT_FOUND;
    bool high = active != ((pin->flags & HAL_PIN_ACTIVE_LOW) != 0);
    if (pin->controller == NULL) {
        gpio_num_t gpio = (gpio_num_t)pin->number;
        if (gpio_set_direction(gpio, GPIO_MODE_OUTPUT) != ESP_OK) return BRUCE_ERR_INVALID_ARGUMENT;
        return gpio_set_level(gpio, high) == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
    }
    const hal_gpio_api_t *api = hal_gpio__api(pin);
    if (api == NULL || api->set_output == NULL) return BRUCE_ERR_NOT_FOUND;
    return api->set_output(pin->controller, pin->number, high);
}

bruce_result_t hal_pin_get(const hal_pin_t *pin, bool *active) {
    if (pin == NULL || active == NULL) return BRUCE_ERR_NOT_FOUND;
    bool high = false;
    if (pin->controller == NULL) {
        high = gpio_get_level((gpio_num_t)pin->number) != 0;
    } else {
        const hal_gpio_api_t *api = hal_gpio__api(pin);
        if (api == NULL || api->get == NULL) return BRUCE_ERR_NOT_FOUND;
        bruce_result_t result = api->get(pin->controller, pin->number, &high);
        if (result != BRUCE_OK) return result;
    }
    *active = high != ((pin->flags & HAL_PIN_ACTIVE_LOW) != 0);
    return BRUCE_OK;
}

int hal_pin_soc_gpio(const hal_pin_t *pin) { return pin != NULL && pin->controller == NULL ? pin->number : -1; }
