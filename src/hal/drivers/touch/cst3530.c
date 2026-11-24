#include "cst3530.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_gpio.h"
#include "hal/hal_i2c.h"

#define CST3530_SPEED_HZ 400000
#define CST3530_TIMEOUT_MS 50
#define CST3530_RESET_MS 10
#define CST3530_BOOT_MS 150
#define CST3530_CHECK_MS 1000 /* Reads anyway this often, in case an edge was missed. */

typedef struct {
    i2c_master_dev_handle_t i2c;
    TickType_t ready_at; /* Out of reset and booted. */
    bool configured;
    const hal_pin_t *irq;
    bool touched; /* The last report had a finger or key: read every call until it ends. */
    TickType_t checked_at;
} cst3530_t;

/* Pulses reset; the chip is set up on the first read, once it has booted. */
static bruce_result_t cst3530__probe(const hal_device_t *device) {
    cst3530_t *chip = calloc(1, sizeof(*chip));
    if (chip == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = hal_i2c_open(device, CST3530_SPEED_HZ, &chip->i2c);
    if (result != BRUCE_OK) {
        free(chip);
        return result;
    }
    const hal_pin_t *reset = hal_device_pin(device, "reset");
    if (reset != NULL) {
        (void)hal_pin_set(reset, true);
        vTaskDelay(pdMS_TO_TICKS(CST3530_RESET_MS));
        (void)hal_pin_set(reset, false);
        chip->ready_at = xTaskGetTickCount() + pdMS_TO_TICKS(CST3530_BOOT_MS);
    }
    const hal_pin_t *irq = hal_device_pin(device, "interrupt");
    if (hal_pin_irq_enable(irq) == BRUCE_OK) chip->irq = irq;
    device->state->data = chip;
    return BRUCE_OK;
}

/* Normal reporting mode, as Hynitron's driver sets it. */
static void cst3530__configure(cst3530_t *chip) {
    static const uint8_t normal_mode[][4] = {
        {0xD0, 0x00, 0x04, 0x00}, {0xD0, 0x00, 0x04, 0x00}, {0xD0, 0x00, 0x00, 0x00},
        {0xD0, 0x00, 0x0C, 0x00}, {0xD0, 0x00, 0x01, 0x00},
    };
    for (size_t i = 0; i < sizeof(normal_mode) / sizeof(normal_mode[0]); ++i) {
        (void)i2c_master_transmit(chip->i2c, normal_mode[i], sizeof(normal_mode[i]), CST3530_TIMEOUT_MS);
        vTaskDelay(1); /* At least 1 ms: one tick, since pdMS_TO_TICKS(1) is 0 at 100 Hz. */
    }
    chip->configured = true;
}

/* Report (Hynitron's hyn_cst66xx.c): a 4-byte header (16-bit checksum,
 * type 0xFF for points, key/finger counts), then 5 bytes per key and
 * finger. Keys come first; only the first finger is used. The controller
 * also answers with other frame types and stale data, which the type and
 * checksum reject. */
static bruce_result_t cst3530__read(const hal_device_t *device, hal_touch_report_t *out) {
    cst3530_t *chip = device->state->data;
    if (!chip->configured) {
        if ((int32_t)(xTaskGetTickCount() - chip->ready_at) < 0) return BRUCE_OK;
        cst3530__configure(chip);
    }
    /* INT pulses low for each report (Hynitron's driver reads on its falling edge). */
    TickType_t now = xTaskGetTickCount();
    if (chip->irq != NULL && !chip->touched && !hal_pin_irq_take(chip->irq) &&
        now - chip->checked_at < pdMS_TO_TICKS(CST3530_CHECK_MS)) {
        return BRUCE_OK;
    }
    chip->checked_at = now;
    chip->touched = false;
    static const uint8_t report_reg[] = {0xD0, 0x07, 0x00, 0x00};
    static const uint8_t report_ack[] = {0xD0, 0x00, 0x02, 0xAB};
    uint8_t report[4 + 5 * 5] = {0};
    /* Separate write and read, with a stop between them, as Hynitron's driver does. */
    if (i2c_master_transmit(chip->i2c, report_reg, sizeof(report_reg), CST3530_TIMEOUT_MS) != ESP_OK ||
        i2c_master_receive(chip->i2c, report, sizeof(report), CST3530_TIMEOUT_MS) != ESP_OK) {
        return BRUCE_ERR_IO;
    }
    (void)i2c_master_transmit(chip->i2c, report_ack, sizeof(report_ack), CST3530_TIMEOUT_MS);

    int fingers = report[3] & 0x0F;
    int keys = report[3] >> 4;
    if (report[2] != 0xFF || keys + fingers == 0 || keys + fingers > 5) return BRUCE_OK;
    uint16_t sum = 0x55;
    for (int i = 4; i < 4 + (keys + fingers) * 5; ++i) sum += report[i];
    if (sum != (report[0] | report[1] << 8)) return BRUCE_OK;

    /* Key byte: id in the low nibble, 1 = pressed in the high nibble. */
    chip->touched = true;
    if (keys > 0 && (report[8] >> 4) != 0) out->key = report[8] & 0x0F;
    if (fingers == 0) return BRUCE_OK;
    const uint8_t *point = &report[4 + keys * 5];
    if ((point[4] >> 4) == 0) return BRUCE_OK;

    int32_t x = point[0] | ((int32_t)(point[3] & 0x0F) << 8);
    int32_t y = point[1] | ((int32_t)(point[3] & 0xF0) << 4);
    const cst3530_config_t *config = device->config;
    hal_touch_orient(config != NULL ? &config->orientation : NULL, &x, &y);
    out->pressed = true;
    out->x = x;
    out->y = y;
    return BRUCE_OK;
}

static const hal_touch_api_t cst3530__api = {.read = cst3530__read};

const hal_driver_t cst3530_driver = {
    .name = "cst3530",
    .class_id = HAL_CLASS_TOUCH,
    .probe = cst3530__probe,
    .api = &cst3530__api,
};
