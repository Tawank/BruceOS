#include "xl9555.h"

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_gpio.h"
#include "hal/hal_i2c.h"

/* Registers 0x00/0x01 read the inputs, 0x02/0x03 are the output ports,
 * 0x06/0x07 the direction (0 = output). */
#define XL9555_INPUT 0x00
#define XL9555_OUTPUT 0x02
#define XL9555_DIRECTION 0x06
#define XL9555_SPEED_HZ 400000
#define XL9555_TIMEOUT_MS 100

typedef struct {
    i2c_master_dev_handle_t i2c;
    uint16_t levels;
    bool kept; /* Set up before an ESP reset: its outputs were left as they were. */
    portMUX_TYPE mux;
} xl9555_t;

static bool xl9555__write(xl9555_t *chip, uint8_t reg, uint16_t value) {
    uint8_t data[] = {reg, (uint8_t)value, (uint8_t)(value >> 8)};
    return i2c_master_transmit(chip->i2c, data, sizeof(data), XL9555_TIMEOUT_MS) == ESP_OK;
}

static bool xl9555__read(xl9555_t *chip, uint8_t reg, uint16_t *value) {
    uint8_t data[2];
    if (i2c_master_transmit_receive(chip->i2c, &reg, 1, data, sizeof(data), XL9555_TIMEOUT_MS) != ESP_OK) return false;
    *value = (uint16_t)(data[1] << 8 | data[0]);
    return true;
}

static bruce_result_t xl9555__probe(const hal_device_t *device) {
    const xl9555_config_t *config = device->config;
    xl9555_t *chip = calloc(1, sizeof(*chip));
    if (chip == NULL) return BRUCE_ERR_NO_MEMORY;
    chip->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    chip->levels = config != NULL ? config->levels : 0;
    bruce_result_t result = hal_i2c_open(device, XL9555_SPEED_HZ, &chip->i2c);
    /* Every pin powers up as an input, so all outputs means an earlier boot
     * set it up and only the ESP restarted: keep what it drives (a modem or a
     * sensor's firmware stays up); their drivers reset what they need to. */
    uint16_t direction = 0xFFFF;
    chip->kept = result == BRUCE_OK && xl9555__read(chip, XL9555_DIRECTION, &direction) && direction == 0 &&
                 xl9555__read(chip, XL9555_OUTPUT, &chip->levels);
    bool ok = result == BRUCE_OK;
    if (ok && !chip->kept) {
        ok = xl9555__write(chip, XL9555_OUTPUT, 0) && xl9555__write(chip, XL9555_DIRECTION, 0);
        if (ok) {
            vTaskDelay(pdMS_TO_TICKS(20));
            ok = xl9555__write(chip, XL9555_OUTPUT, chip->levels);
        }
    }
    if (!ok) {
        if (chip->i2c != NULL) i2c_master_bus_rm_device(chip->i2c);
        free(chip);
        return result == BRUCE_OK ? BRUCE_ERR_NOT_FOUND : result;
    }
    device->state->data = chip;
    return BRUCE_OK;
}

static bruce_result_t xl9555__set_output(const hal_device_t *device, uint8_t number, bool high) {
    xl9555_t *chip = device->state->data;
    if (number > 15) return BRUCE_ERR_INVALID_ARGUMENT;
    portENTER_CRITICAL(&chip->mux);
    if (high) chip->levels |= (uint16_t)(1u << number);
    else chip->levels &= (uint16_t)~(1u << number);
    uint16_t levels = chip->levels;
    portEXIT_CRITICAL(&chip->mux);
    return xl9555__write(chip, XL9555_OUTPUT, levels) ? BRUCE_OK : BRUCE_ERR_IO;
}

static bruce_result_t xl9555__get(const hal_device_t *device, uint8_t number, bool *high) {
    xl9555_t *chip = device->state->data;
    if (number > 15) return BRUCE_ERR_INVALID_ARGUMENT;
    uint8_t reg = XL9555_INPUT;
    uint8_t value[2];
    if (i2c_master_transmit_receive(chip->i2c, &reg, 1, value, sizeof(value), XL9555_TIMEOUT_MS) != ESP_OK) {
        return BRUCE_ERR_IO;
    }
    *high = (((uint16_t)value[1] << 8 | value[0]) >> number) & 1u;
    return BRUCE_OK;
}

static void xl9555__describe(const hal_device_t *device, char *out, size_t size) {
    const xl9555_t *chip = device->state->data;
    snprintf(out, size, "outputs 0x%04x at probe%s", chip->levels, chip->kept ? ", kept through a restart" : "");
}

static const hal_gpio_api_t xl9555__api = {.set_output = xl9555__set_output, .get = xl9555__get};

const hal_driver_t xl9555_driver = {
    .name = "xl9555",
    .class_id = HAL_CLASS_GPIO,
    .probe = xl9555__probe,
    .describe = xl9555__describe,
    .api = &xl9555__api,
};
