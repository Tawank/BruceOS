#include "hal_rail.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal_gpio.h"

/* Off until someone uses it. */
static bruce_result_t hal_rail__probe(const hal_device_t *device) {
    const hal_rail_config_t *config = device->config;
    return hal_pin_set(&config->enable, false);
}

/* Called by hal_device_power() under the HAL lock. */
bruce_result_t hal_rail__use(const hal_device_t *rail, bool on) {
    if (!hal_device_ready(rail) || rail->driver != &hal_rail_driver) return BRUCE_ERR_NOT_FOUND;
    const hal_rail_config_t *config = rail->config;
    hal_device_state_t *state = rail->state;
    if (!on) {
        if (state->users == 0) return BRUCE_OK;
        return --state->users == 0 ? hal_pin_set(&config->enable, false) : BRUCE_OK;
    }
    if (state->users++ > 0) return BRUCE_OK;
    bruce_result_t result = hal_pin_set(&config->enable, true);
    if (result == BRUCE_OK && config->on_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(config->on_delay_ms));
    return result;
}

const hal_driver_t hal_rail_driver = {
    .name = "rail",
    .class_id = HAL_CLASS_RAIL,
    .probe = hal_rail__probe,
};
