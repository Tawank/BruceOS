#pragma once

#include "driver/ledc.h"
#include "hal/hal.h"

/* An LED dimmed by PWM on its SoC "led" pin (Linux's leds-pwm). The board
 * gives it an LEDC timer and channel no other user has. */
typedef struct {
    ledc_timer_t timer;
    ledc_channel_t channel;
    uint32_t freq_hz;
} pwm_led_config_t;

extern const hal_driver_t pwm_led_driver;
