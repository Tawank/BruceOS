#pragma once

#include "core_sdk/result.h"

/**
 * @brief GPIO pin access.
 */

typedef enum {
    BRUCE_GPIO_MODE_INPUT = 0,
    BRUCE_GPIO_MODE_OUTPUT,
    BRUCE_GPIO_MODE_INPUT_OUTPUT,
} bruce_gpio_mode_t;

typedef enum {
    BRUCE_GPIO_PULL_NONE = 0,
    BRUCE_GPIO_PULL_UP,
    BRUCE_GPIO_PULL_DOWN,
} bruce_gpio_pull_t;

/**
 * @brief Configures a GPIO pin's mode and pull resistor.
 *
 * Pins and modes are checked against the selected SoC before the ESP-IDF
 * driver is called.
 *
 * @param pin GPIO pin number.
 * @param mode Pin direction mode.
 * @param pull Pull resistor configuration.
 * @permission gpio
 */
bruce_result_t gpio__configure(int pin, bruce_gpio_mode_t mode, bruce_gpio_pull_t pull);

/**
 * @brief Reads a GPIO pin's logic level.
 *
 * @param pin GPIO pin number.
 * @param out_level Receives the pin's level (0 or 1).
 * @permission gpio
 */
bruce_result_t gpio__read(int pin, int *out_level);

/**
 * @brief Sets a GPIO pin's output logic level.
 *
 * @param pin GPIO pin number.
 * @param level New level (0 or 1).
 * @permission gpio
 */
bruce_result_t gpio__write(int pin, int level);
