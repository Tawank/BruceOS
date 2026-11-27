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

#define HAL_GPIO__IRQ_PINS 8

static volatile bool s_irq_fired[GPIO_NUM_MAX];
static bool s_irq_level[GPIO_NUM_MAX]; /* Level-triggered (wakes sleep), else edge. */
static uint8_t s_irq_pins[HAL_GPIO__IRQ_PINS];
static gpio_int_type_t s_irq_levels[HAL_GPIO__IRQ_PINS];
static size_t s_irq_count;
static void (*s_irq_hook)(void);

/* A level interrupt is masked until taken, or it would fire for as long as the line stays active. */
static void IRAM_ATTR hal_gpio__isr(void *arg) {
    gpio_num_t gpio = (gpio_num_t)(uintptr_t)arg;
    if (s_irq_level[gpio]) gpio_intr_disable(gpio);
    s_irq_fired[gpio] = true;
    void (*hook)(void) = s_irq_hook;
    if (hook != NULL) hook();
}

void hal_pin_irq_set_hook(void (*hook)(void)) { s_irq_hook = hook; }

bruce_result_t hal_pin_irq_enable(const hal_pin_t *pin, bool wake) {
    if (pin == NULL) return BRUCE_ERR_NOT_FOUND;
    if (pin->controller != NULL || pin->number >= GPIO_NUM_MAX) return BRUCE_ERR_UNSUPPORTED;
    if (s_irq_count >= HAL_GPIO__IRQ_PINS) return BRUCE_ERR_RESOURCE_LIMIT;
    bool active_low = (pin->flags & HAL_PIN_ACTIVE_LOW) != 0;
    gpio_int_type_t level = active_low ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL;
    gpio_int_type_t edge = active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin->number,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .intr_type = wake ? level : edge,
    };
    s_irq_level[pin->number] = wake;
    if (gpio_config(&config) != ESP_OK) return BRUCE_ERR_INVALID_ARGUMENT;
    static bool s_isr_service; /* A second install logs an error. */
    if (!s_isr_service) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return BRUCE_ERR_IO;
        s_isr_service = true;
    }
    s_irq_fired[pin->number] = true; /* Read once, for anything that came before. */
    if (gpio_isr_handler_add((gpio_num_t)pin->number, hal_gpio__isr, (void *)(uintptr_t)pin->number) != ESP_OK) {
        return BRUCE_ERR_IO;
    }
    if (!wake) return BRUCE_OK;
    (void)gpio_wakeup_enable((gpio_num_t)pin->number, level);
    s_irq_pins[s_irq_count] = pin->number;
    s_irq_levels[s_irq_count++] = level;
    return BRUCE_OK;
}

bool hal_pin_irq_take(const hal_pin_t *pin) {
    if (pin == NULL || pin->controller != NULL || pin->number >= GPIO_NUM_MAX || !s_irq_fired[pin->number]) return false;
    s_irq_fired[pin->number] = false;
    /* A level interrupt fires again at once if the line is still active. */
    if (s_irq_level[pin->number]) gpio_intr_enable((gpio_num_t)pin->number);
    return true;
}

/* gpio_wakeup_disable() leaves the level interrupt type alone, so the pin keeps working. */
void hal_pin_irq_wake_all(bool on) {
    for (size_t i = 0; i < s_irq_count; ++i) {
        gpio_num_t gpio = (gpio_num_t)s_irq_pins[i];
        if (on) {
            (void)gpio_wakeup_enable(gpio, s_irq_levels[i]);
        } else {
            (void)gpio_wakeup_disable(gpio);
        }
    }
}
