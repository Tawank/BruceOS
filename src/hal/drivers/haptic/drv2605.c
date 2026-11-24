#include "drv2605.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_haptic.h"
#include "hal/hal_i2c.h"

/* Registers and the ERM open-loop setup as Adafruit's DRV2605 library does
 * it (LilyGO's T-Deck MAX motor example uses it); effect numbers from TI's
 * ROM library table, as in that example. */
#define DRV2605_STATUS 0x00
#define DRV2605_MODE 0x01
#define DRV2605_RTP_INPUT 0x02
#define DRV2605_LIBRARY 0x03
#define DRV2605_WAVESEQ1 0x04
#define DRV2605_WAVESEQ2 0x05
#define DRV2605_GO 0x0C
#define DRV2605_OVERDRIVE 0x0D
#define DRV2605_SUSTAIN_POS 0x0E
#define DRV2605_SUSTAIN_NEG 0x0F
#define DRV2605_BRAKE 0x10
#define DRV2605_AUDIO_MAX 0x13
#define DRV2605_FEEDBACK 0x1A
#define DRV2605_CONTROL3 0x1D
#define DRV2605_MODE_INTERNAL_TRIGGER 0x00
#define DRV2605_N_ERM_LRA 0x80
#define DRV2605_ERM_OPEN_LOOP 0x20
#define DRV2605_LIBRARY_ERM_A 1
#define DRV2605_SPEED_HZ 400000
#define DRV2605_TIMEOUT_MS 50
#define DRV2605_READY_TRIES 5 /* One tick apart, after power-up. */

static bool drv2605__write(i2c_master_dev_handle_t i2c, uint8_t reg, uint8_t value) {
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(i2c, data, sizeof(data), DRV2605_TIMEOUT_MS) == ESP_OK;
}

static bool drv2605__read(i2c_master_dev_handle_t i2c, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(i2c, &reg, 1, value, 1, DRV2605_TIMEOUT_MS) == ESP_OK;
}

/* Until it answers after power-up. */
static bool drv2605__wait_ready(i2c_master_dev_handle_t i2c) {
    uint8_t status = 0;
    for (int i = 0; i < DRV2605_READY_TRIES; ++i) {
        if (drv2605__read(i2c, DRV2605_STATUS, &status)) return true;
        vTaskDelay(1);
    }
    return false;
}

/* Checks the chip answers, then powers it off until used. */
static bruce_result_t drv2605__probe(const hal_device_t *device) {
    i2c_master_dev_handle_t i2c = NULL;
    bruce_result_t result = hal_i2c_open(device, DRV2605_SPEED_HZ, &i2c);
    if (result != BRUCE_OK) return result;
    result = hal_device_power(device, true);
    if (result == BRUCE_OK && !drv2605__wait_ready(i2c)) result = BRUCE_ERR_NOT_FOUND;
    (void)hal_device_power(device, false);
    if (result != BRUCE_OK) {
        i2c_master_bus_rm_device(i2c);
        return result;
    }
    device->state->data = i2c;
    return BRUCE_OK;
}

/* Powered fresh each time, so set up each time: out of standby, ERM open loop, ROM library, internal trigger. */
static bruce_result_t drv2605__start(const hal_device_t *device) {
    i2c_master_dev_handle_t i2c = device->state->data;
    bruce_result_t result = hal_device_power(device, true);
    if (result != BRUCE_OK) return result;
    uint8_t feedback = 0, control3 = 0;
    bool ok = drv2605__wait_ready(i2c) && drv2605__write(i2c, DRV2605_MODE, 0x00) && drv2605__write(i2c, DRV2605_RTP_INPUT, 0) &&
              drv2605__write(i2c, DRV2605_OVERDRIVE, 0) && drv2605__write(i2c, DRV2605_SUSTAIN_POS, 0) &&
              drv2605__write(i2c, DRV2605_SUSTAIN_NEG, 0) && drv2605__write(i2c, DRV2605_BRAKE, 0) &&
              drv2605__write(i2c, DRV2605_AUDIO_MAX, 0x64) && drv2605__read(i2c, DRV2605_FEEDBACK, &feedback) &&
              drv2605__write(i2c, DRV2605_FEEDBACK, feedback & (uint8_t)~DRV2605_N_ERM_LRA) &&
              drv2605__read(i2c, DRV2605_CONTROL3, &control3) &&
              drv2605__write(i2c, DRV2605_CONTROL3, control3 | DRV2605_ERM_OPEN_LOOP) &&
              drv2605__write(i2c, DRV2605_LIBRARY, DRV2605_LIBRARY_ERM_A) &&
              drv2605__write(i2c, DRV2605_MODE, DRV2605_MODE_INTERNAL_TRIGGER);
    if (!ok) {
        (void)hal_device_power(device, false);
        return BRUCE_ERR_IO;
    }
    return BRUCE_OK;
}

static void drv2605__stop(const hal_device_t *device) { (void)hal_device_power(device, false); }

static bruce_result_t drv2605__play(const hal_device_t *device, hal_haptic_effect_t effect) {
    /* 1 Strong Click, 10 Double Click, 47 Buzz 1, 15 750 ms Alert (all 100%). */
    static const uint8_t effects[] = {
        [HAL_HAPTIC_CLICK] = 1, [HAL_HAPTIC_DOUBLE_CLICK] = 10, [HAL_HAPTIC_BUZZ] = 47, [HAL_HAPTIC_ALERT] = 15};
    if ((unsigned)effect >= sizeof(effects)) return BRUCE_ERR_INVALID_ARGUMENT;
    i2c_master_dev_handle_t i2c = device->state->data;
    bool ok = drv2605__write(i2c, DRV2605_WAVESEQ1, effects[effect]) && drv2605__write(i2c, DRV2605_WAVESEQ2, 0) &&
              drv2605__write(i2c, DRV2605_GO, 1);
    return ok ? BRUCE_OK : BRUCE_ERR_IO;
}

static const hal_haptic_api_t drv2605__api = {.play = drv2605__play};

const hal_driver_t drv2605_driver = {
    .name = "drv2605",
    .class_id = HAL_CLASS_HAPTIC,
    .probe = drv2605__probe,
    .start = drv2605__start,
    .stop = drv2605__stop,
    .api = &drv2605__api,
};
