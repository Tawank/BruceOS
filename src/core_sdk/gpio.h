#pragma once

#include <stdint.h>

#include "core_sdk/result.h"

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

/* Raw GPIO access requires the `gpio` permission. Pins and modes are checked
 * against the selected SoC before the ESP-IDF driver is called. */
bruce_result_t gpio__configure(int pin, bruce_gpio_mode_t mode, bruce_gpio_pull_t pull);
bruce_result_t gpio__read(int pin, int *out_level);
bruce_result_t gpio__write(int pin, int level);
