#include "tca8418.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_gpio.h"
#include "hal/hal_i2c.h"

#define TCA8418_CFG 0x01
#define TCA8418_INT_STAT 0x02
#define TCA8418_KEY_LCK_EC 0x03
#define TCA8418_KEY_EVENT_A 0x04
#define TCA8418_KP_GPIO1 0x1D
#define TCA8418_KP_GPIO2 0x1E
#define TCA8418_KP_GPIO3 0x1F
#define TCA8418_SPEED_HZ 400000
#define TCA8418_TIMEOUT_MS 20
#define TCA8418_RESET_MS 10
#define TCA8418_CHECK_MS 1000 /* Reads anyway this often, in case an edge was missed. */

typedef struct {
    i2c_master_dev_handle_t i2c;
    bool configured;
    uint8_t pending; /* Events left of the count last read. */
    const hal_pin_t *irq;
    TickType_t checked_at;
    bool draining; /* Events were read: look again until the FIFO is empty and INT cleared. */
} tca8418_t;

static bool tca8418__write(tca8418_t *chip, uint8_t reg, uint8_t value) {
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(chip->i2c, data, sizeof(data), TCA8418_TIMEOUT_MS) == ESP_OK;
}

static bool tca8418__read(tca8418_t *chip, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(chip->i2c, &reg, 1, value, 1, TCA8418_TIMEOUT_MS) == ESP_OK;
}

/* Pulses reset; the matrix is set up on the first read. */
static bruce_result_t tca8418__probe(const hal_device_t *device) {
    if (device->config == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    tca8418_t *chip = calloc(1, sizeof(*chip));
    if (chip == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = hal_i2c_open(device, TCA8418_SPEED_HZ, &chip->i2c);
    if (result != BRUCE_OK) {
        free(chip);
        return result;
    }
    const hal_pin_t *reset = hal_device_pin(device, "reset");
    if (reset != NULL) {
        (void)hal_pin_set(reset, true);
        vTaskDelay(pdMS_TO_TICKS(TCA8418_RESET_MS));
        (void)hal_pin_set(reset, false);
    }
    const hal_pin_t *irq = hal_device_pin(device, "interrupt");
    if (hal_pin_irq_enable(irq) == BRUCE_OK) chip->irq = irq;
    device->state->data = chip;
    return BRUCE_OK;
}

/* Selects the matrix, drops events from power-on and enables the key
 * interrupt (INT low while K_INT is set). */
static bool tca8418__configure(tca8418_t *chip, const tca8418_config_t *config) {
    if (!tca8418__write(chip, TCA8418_KP_GPIO1, config->rows) ||
        !tca8418__write(chip, TCA8418_KP_GPIO2, (uint8_t)config->cols) ||
        !tca8418__write(chip, TCA8418_KP_GPIO3, (uint8_t)(config->cols >> 8))) {
        return false;
    }
    uint8_t count = 0;
    if (tca8418__read(chip, TCA8418_KEY_LCK_EC, &count)) {
        count &= 0x0F;
        while (count-- > 0) {
            uint8_t ignored;
            if (!tca8418__read(chip, TCA8418_KEY_EVENT_A, &ignored)) break;
        }
    }
    return tca8418__write(chip, TCA8418_INT_STAT, 0x03) && tca8418__write(chip, TCA8418_CFG, 0x01);
}

/* KEY_EVENT_A: bit 7 pressed, bits 0-6 the key number, row * 10 + col + 1. */
static bruce_result_t tca8418__next_event(const hal_device_t *device, hal_keypad_event_t *out) {
    tca8418_t *chip = device->state->data;
    if (!chip->configured) {
        if (!tca8418__configure(chip, device->config)) return BRUCE_ERR_IO;
        chip->configured = true;
    }
    if (chip->pending == 0) {
        /* Only on an interrupt or while draining; with no interrupt pin, every call. */
        TickType_t now = xTaskGetTickCount();
        if (chip->irq != NULL && !chip->draining && !hal_pin_irq_take(chip->irq) &&
            now - chip->checked_at < pdMS_TO_TICKS(TCA8418_CHECK_MS)) {
            return BRUCE_ERR_NOT_FOUND;
        }
        uint8_t count = 0;
        if (!tca8418__read(chip, TCA8418_KEY_LCK_EC, &count)) return BRUCE_ERR_IO;
        chip->checked_at = now;
        chip->pending = count & 0x0F;
        if (chip->pending == 0) {
            /* TI's order: K_INT and INT clear only once the FIFO is empty; a
             * key that came meanwhile is counted again after the clear. */
            if (!tca8418__write(chip, TCA8418_INT_STAT, 0x01) ||
                !tca8418__read(chip, TCA8418_KEY_LCK_EC, &count)) {
                return BRUCE_ERR_IO;
            }
            chip->pending = count & 0x0F;
            chip->draining = chip->pending > 0;
            if (chip->pending == 0) return BRUCE_ERR_NOT_FOUND;
        }
        chip->draining = true;
    }
    uint8_t event = 0;
    if (!tca8418__read(chip, TCA8418_KEY_EVENT_A, &event) || (event & 0x7F) == 0) {
        chip->pending = 0;
        return BRUCE_ERR_NOT_FOUND;
    }
    chip->pending--;
    uint8_t key = (uint8_t)((event & 0x7F) - 1);
    *out = (hal_keypad_event_t){.row = key / 10, .col = key % 10, .pressed = (event & 0x80) != 0};
    return BRUCE_OK;
}

static const hal_keypad_api_t tca8418__api = {.next_event = tca8418__next_event};

const hal_driver_t tca8418_driver = {
    .name = "tca8418",
    .class_id = HAL_CLASS_KEYPAD,
    .probe = tca8418__probe,
    .api = &tca8418__api,
};
