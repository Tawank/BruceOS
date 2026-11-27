#pragma once

#include "hal.h"

/* Operations of a HAL_CLASS_GPIO driver, on raw (electrical) levels. */
typedef struct {
    bruce_result_t (*set_output)(const hal_device_t *controller, uint8_t number, bool high);
    bruce_result_t (*get)(const hal_device_t *controller, uint8_t number, bool *high);
} hal_gpio_api_t;

/* Makes the pin an output and drives it active or inactive. NULL pin: BRUCE_ERR_NOT_FOUND. */
bruce_result_t hal_pin_set(const hal_pin_t *pin, bool active);
bruce_result_t hal_pin_get(const hal_pin_t *pin, bool *active);
/* The SoC GPIO number, or -1 when the pin is on another controller. */
int hal_pin_soc_gpio(const hal_pin_t *pin);

/* Interrupts on a SoC pin's active level (pulled up when active-low), for
 * hal_pin_irq_take(). The pin also wakes light sleep. Pins on other
 * controllers: BRUCE_ERR_UNSUPPORTED. */
bruce_result_t hal_pin_irq_enable(const hal_pin_t *pin);
/* Whether the pin was active since the last call; the interrupt is masked
 * until then. Read the chip after this, so nothing during the read is lost. */
bool hal_pin_irq_take(const hal_pin_t *pin);
/* Called from the interrupt on any such pin (keep it short, ISR-safe). */
void hal_pin_irq_set_hook(void (*hook)(void));
/* Whether these pins wake light sleep (on after hal_pin_irq_enable()). */
void hal_pin_irq_wake_all(bool on);
