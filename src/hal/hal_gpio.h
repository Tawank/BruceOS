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

/* Notes each active edge of a SoC pin (pulled up when active-low), for
 * hal_pin_irq_take(). Pins on other controllers: BRUCE_ERR_UNSUPPORTED. */
bruce_result_t hal_pin_irq_enable(const hal_pin_t *pin);
/* Whether an active edge came since the last call. Read the chip after
 * this, so an edge during the read is not lost. */
bool hal_pin_irq_take(const hal_pin_t *pin);
