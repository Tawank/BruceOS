#include "hal_gpio.h"

#include "driver/gpio.h"
#include "esp_attr.h"

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

static volatile bool s_irq_fired[GPIO_NUM_MAX];

static void IRAM_ATTR hal_gpio__isr(void *arg) { s_irq_fired[(uintptr_t)arg] = true; }

bruce_result_t hal_pin_irq_enable(const hal_pin_t *pin) {
    if (pin == NULL) return BRUCE_ERR_NOT_FOUND;
    if (pin->controller != NULL || pin->number >= GPIO_NUM_MAX) return BRUCE_ERR_UNSUPPORTED;
    bool active_low = (pin->flags & HAL_PIN_ACTIVE_LOW) != 0;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin->number,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .intr_type = active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    if (gpio_config(&config) != ESP_OK) return BRUCE_ERR_INVALID_ARGUMENT;
    static bool s_isr_service; /* A second install logs an error. */
    if (!s_isr_service) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return BRUCE_ERR_IO;
        s_isr_service = true;
    }
    s_irq_fired[pin->number] = true; /* Read once, for anything that came before. */
    return gpio_isr_handler_add((gpio_num_t)pin->number, hal_gpio__isr, (void *)(uintptr_t)pin->number) == ESP_OK
               ? BRUCE_OK
               : BRUCE_ERR_IO;
}

bool hal_pin_irq_take(const hal_pin_t *pin) {
    if (pin == NULL || pin->controller != NULL || pin->number >= GPIO_NUM_MAX || !s_irq_fired[pin->number]) return false;
    s_irq_fired[pin->number] = false;
    return true;
}
