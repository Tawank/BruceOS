#pragma once

#include "core_sdk/gpio.h"

/* Trusted Core drivers use these without changing their public permission. */
bruce_result_t gpio__configure_internal(int pin, bruce_gpio_mode_t mode, bruce_gpio_pull_t pull);
bruce_result_t gpio__read_internal(int pin, int *out_level);
bruce_result_t gpio__write_internal(int pin, int level);
