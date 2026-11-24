#include "pwm_led.h"

#include "hal/hal_gpio.h"
#include "hal/hal_led.h"

/* Off until set. LEDC_USE_APB_CLK as the display backlight: the S3's timers share one clock source. */
static bruce_result_t pwm_led__probe(const hal_device_t *device) {
    const pwm_led_config_t *config = device->config;
    const hal_pin_t *pin = hal_device_pin(device, "led");
    int gpio = hal_pin_soc_gpio(pin);
    if (config == NULL || gpio < 0) return BRUCE_ERR_INVALID_ARGUMENT;
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = config->timer,
        .freq_hz = config->freq_hz,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = config->channel,
        .timer_sel = config->timer,
        .flags.output_invert = (pin->flags & HAL_PIN_ACTIVE_LOW) != 0,
    };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) return BRUCE_ERR_IO;
    return BRUCE_OK;
}

static bruce_result_t pwm_led__set(const hal_device_t *device, uint8_t brightness) {
    const pwm_led_config_t *config = device->config;
    uint32_t duty = brightness == 255 ? 256 : brightness; /* 256 of 8 bits is always on. */
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, config->channel, duty) != ESP_OK ||
        ledc_update_duty(LEDC_LOW_SPEED_MODE, config->channel) != ESP_OK) {
        return BRUCE_ERR_IO;
    }
    return BRUCE_OK;
}

static const hal_led_api_t pwm_led__api = {.set = pwm_led__set};

const hal_driver_t pwm_led_driver = {
    .name = "pwm-led",
    .class_id = HAL_CLASS_LED,
    .probe = pwm_led__probe,
    .api = &pwm_led__api,
};
