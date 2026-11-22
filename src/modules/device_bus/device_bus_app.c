#include "device_bus_app.h"
#include "core_sdk/result.h"
#include "sdkconfig.h"

#if CONFIG_BRUCE_BOARD_I2C_ENABLED

#include <string.h>

#include "core/device/board_i2c.h"
#include "core/device/device.h"

#include "core_sdk/device.h"
#include "core_sdk/process.h"
#include "core_sdk/pubsub.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define DEVICE_BUS__POLL_PERIOD_MS 20
#define DEVICE_BUS__BATTERY_POLL_PERIOD_MS 30000
#define DEVICE_BUS__BATTERY_RETRY_MS 2000

#if CONFIG_BRUCE_QEMU_TEST_MODE || !CONFIG_BRUCE_BATTERY_ENABLED
#define DEVICE_BUS__NO_I2C_BATTERY 1
#elif CONFIG_BRUCE_BATTERY_BACKEND_AXP2101
#define DEVICE_BUS__BATTERY_BACKEND_AXP2101 1
#elif CONFIG_BRUCE_BATTERY_BACKEND_AXP192
#define DEVICE_BUS__BATTERY_BACKEND_AXP192 1
#elif CONFIG_BRUCE_BATTERY_BACKEND_BQ27220
#define DEVICE_BUS__BATTERY_BACKEND_BQ27220 1
#else
/* The analog ADC backend has no bus-sharing concern and is read directly by
 * device.c instead of through this process. */
#define DEVICE_BUS__NO_I2C_BATTERY 1
#endif

/* ---- Touch: FT5x06/FT6336-family register map (public Focaltech
 * datasheets, ESP-IDF's esp_lcd_touch_ft5x06 component, Linux's
 * edt-ft5x06 driver). Only point 1 is read - single-touch, matching what
 * this HAL has ever reported. ---- */
#if CONFIG_BRUCE_TOUCH_ENABLED

#define DEVICE_BUS__TOUCH_REG_TD_STATUS 0x02
#define DEVICE_BUS__TOUCH_REG_TOUCH1_XH 0x03

static i2c_master_dev_handle_t s_touch_dev;
static bool s_touch_ready;
static bool s_touch_prev_pressed;
static int32_t s_touch_prev_x;
static int32_t s_touch_prev_y;
static int32_t s_touch_prev_key = -1;

static void device_bus__touch_reset(void) {
#if CONFIG_BRUCE_TOUCH_PIN_RST >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_BRUCE_TOUCH_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)CONFIG_BRUCE_TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)CONFIG_BRUCE_TOUCH_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
#endif
}

static void device_bus__touch_apply_orientation(int32_t *x, int32_t *y) {
#if CONFIG_BRUCE_TOUCH_SWAP_XY
    int32_t swapped = *x;
    *x = *y;
    *y = swapped;
#endif
#if CONFIG_BRUCE_TOUCH_MIRROR_X
    *x = CONFIG_BRUCE_DISPLAY_WIDTH - 1 - *x;
#endif
#if CONFIG_BRUCE_TOUCH_MIRROR_Y
    *y = CONFIG_BRUCE_DISPLAY_HEIGHT - 1 - *y;
#endif
}

static bool device_bus__touch_ensure_device(i2c_master_bus_handle_t bus) {
    if (s_touch_ready) return true;
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_TOUCH_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_config, &s_touch_dev) != ESP_OK) return false;
#if CONFIG_BRUCE_TOUCH_CST3530
    /* Normal reporting mode, as Hynitron's driver sets it. */
    static const uint8_t normal_mode[][4] = {
        {0xD0, 0x00, 0x04, 0x00}, {0xD0, 0x00, 0x04, 0x00}, {0xD0, 0x00, 0x00, 0x00},
        {0xD0, 0x00, 0x0C, 0x00}, {0xD0, 0x00, 0x01, 0x00},
    };
    for (size_t i = 0; i < sizeof(normal_mode) / sizeof(normal_mode[0]); ++i) {
        (void)i2c_master_transmit(s_touch_dev, normal_mode[i], sizeof(normal_mode[i]), 50);
        vTaskDelay(1); /* At least 1 ms: one tick, since pdMS_TO_TICKS(1) is 0 at 100 Hz. */
    }
#endif
    s_touch_ready = true;
    return true;
}

#if CONFIG_BRUCE_TOUCH_CST3530
/* Hynitron CST3530/CST66xx report (Hynitron's hyn_cst66xx.c): a 4-byte
 * header (16-bit checksum, type 0xFF for points, key/finger counts), then 5
 * bytes per key and finger. Keys come first; only the first finger is used.
 * The controller also answers with other frame types and stale data, which
 * the type and checksum reject. */
static bool device_bus__touch_read(int32_t *out_x, int32_t *out_y, int32_t *out_key) {
    *out_key = -1;
    static const uint8_t report_reg[] = {0xD0, 0x07, 0x00, 0x00};
    static const uint8_t report_ack[] = {0xD0, 0x00, 0x02, 0xAB};
    uint8_t report[4 + 5 * 5] = {0};
    /* Separate write and read, with a stop between them, as Hynitron's driver does. */
    if (i2c_master_transmit(s_touch_dev, report_reg, sizeof(report_reg), 50) != ESP_OK ||
        i2c_master_receive(s_touch_dev, report, sizeof(report), 50) != ESP_OK) {
        return false;
    }
    (void)i2c_master_transmit(s_touch_dev, report_ack, sizeof(report_ack), 50);

    int fingers = report[3] & 0x0F;
    int keys = report[3] >> 4;
    if (report[2] != 0xFF || keys + fingers == 0 || keys + fingers > 5) return false;
    uint16_t sum = 0x55;
    for (int i = 4; i < 4 + (keys + fingers) * 5; ++i) sum += report[i];
    if (sum != (report[0] | report[1] << 8)) return false;

    /* Key byte: id in the low nibble, 1 = pressed in the high nibble. */
    if (keys > 0 && (report[8] >> 4) != 0) *out_key = report[8] & 0x0F;
    if (fingers == 0) return false;
    const uint8_t *point = &report[4 + keys * 5];
    if ((point[4] >> 4) == 0) return false;

    int32_t x = point[0] | ((int32_t)(point[3] & 0x0F) << 8);
    int32_t y = point[1] | ((int32_t)(point[3] & 0xF0) << 4);
    device_bus__touch_apply_orientation(&x, &y);
    *out_x = x;
    *out_y = y;
    return true;
}
#else
static bool device_bus__touch_read(int32_t *out_x, int32_t *out_y, int32_t *out_key) {
    *out_key = -1;
    uint8_t status_reg = DEVICE_BUS__TOUCH_REG_TD_STATUS;
    uint8_t status = 0;
    if (i2c_master_transmit_receive(s_touch_dev, &status_reg, 1, &status, 1, -1) != ESP_OK) return false;
    if ((status & 0x0F) == 0) return false;

    uint8_t point_reg = DEVICE_BUS__TOUCH_REG_TOUCH1_XH;
    uint8_t point[4] = {0};
    if (i2c_master_transmit_receive(s_touch_dev, &point_reg, 1, point, sizeof(point), -1) != ESP_OK) {
        return false;
    }

    int32_t x = ((int32_t)(point[0] & 0x0F) << 8) | point[1];
    int32_t y = ((int32_t)(point[2] & 0x0F) << 8) | point[3];
    device_bus__touch_apply_orientation(&x, &y);
    *out_x = x;
    *out_y = y;
    return true;
}
#endif

static void device_bus__touch_publish(bruce_input_action_t action, int32_t x, int32_t y, int32_t key) {
    bruce_device_touch_message_t message = {.action = action, .x = x, .y = y, .key = key};
    (void)pubsub__publish(BRUCE_DEVICE_TOPIC_TOUCH, &message, sizeof(message));
}

static void device_bus__touch_poll(i2c_master_bus_handle_t bus) {
    if (!device_bus__touch_ensure_device(bus)) return;

    int32_t x = 0;
    int32_t y = 0;
    int32_t key = -1;
    bool pressed = device_bus__touch_read(&x, &y, &key);

    if (pressed && !s_touch_prev_pressed) {
        device_bus__touch_publish(BRUCE_INPUT_PRESS, x, y, -1);
    } else if (pressed && s_touch_prev_pressed && (x != s_touch_prev_x || y != s_touch_prev_y)) {
        device_bus__touch_publish(BRUCE_INPUT_CHANGE, x, y, -1);
    } else if (!pressed && s_touch_prev_pressed) {
        device_bus__touch_publish(BRUCE_INPUT_RELEASE, s_touch_prev_x, s_touch_prev_y, -1);
    }

    if (key != s_touch_prev_key) {
        if (s_touch_prev_key >= 0) device_bus__touch_publish(BRUCE_INPUT_RELEASE, 0, 0, s_touch_prev_key);
        if (key >= 0) device_bus__touch_publish(BRUCE_INPUT_PRESS, 0, 0, key);
        s_touch_prev_key = key;
    }

    s_touch_prev_pressed = pressed;
    if (pressed) {
        s_touch_prev_x = x;
        s_touch_prev_y = y;
    }
}

#endif /* CONFIG_BRUCE_TOUCH_ENABLED */

/* ---- I2C battery backends. Register maps verified against public
 * datasheets/reference implementations: AXP2101/AXP192 per M5Unified's
 * AXP2101_Class.cpp/AXP192_Class.cpp, BQ27220 per TI's BQ27220 Technical
 * Reference Manual (SLUUBD4A). Each backend is polled on the same shared
 * bus this process already owns for touch, so unlike the old lazily-opened
 * device.c implementation, the device handle is created once and kept for
 * this process's lifetime - there is no concurrent caller to guard
 * against. ---- */

#if defined(DEVICE_BUS__BATTERY_BACKEND_AXP2101)

#define DEVICE_BUS__AXP2101_REG_ADC_ENABLE 0x30
#define DEVICE_BUS__AXP2101_ADC_ENABLE_MASK 0x0F
#define DEVICE_BUS__AXP2101_REG_BATTERY_PERCENT 0xA4

static i2c_master_dev_handle_t s_battery_dev;
static bool s_battery_ready;

static bool device_bus__battery_ensure_device(i2c_master_bus_handle_t bus) {
    if (s_battery_ready) return true;
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_BATTERY_AXP2101_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_config, &s_battery_dev) != ESP_OK) return false;

    uint8_t enable_adc[] = {DEVICE_BUS__AXP2101_REG_ADC_ENABLE, DEVICE_BUS__AXP2101_ADC_ENABLE_MASK};
    if (i2c_master_transmit(s_battery_dev, enable_adc, sizeof(enable_adc), -1) != ESP_OK) return false;

    s_battery_ready = true;
    return true;
}

static bool device_bus__battery_read(int *out_percent) {
    uint8_t reg = DEVICE_BUS__AXP2101_REG_BATTERY_PERCENT;
    int8_t raw = 0;
    if (i2c_master_transmit_receive(s_battery_dev, &reg, 1, (uint8_t *)&raw, 1, -1) != ESP_OK) return false;
    int percent = raw;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    *out_percent = percent;
    return true;
}

#elif defined(DEVICE_BUS__BATTERY_BACKEND_AXP192)

#define DEVICE_BUS__AXP192_REG_ADC_ENABLE 0x82
#define DEVICE_BUS__AXP192_ADC_ENABLE_ALL 0xFF
#define DEVICE_BUS__AXP192_REG_BATTERY_VOLTAGE 0x78
#define DEVICE_BUS__AXP192_VOLTAGE_LSB_MV_NUM 11
#define DEVICE_BUS__AXP192_VOLTAGE_LSB_MV_DEN 10
#define DEVICE_BUS__BATTERY_EMPTY_MV 3300
#define DEVICE_BUS__BATTERY_FULL_MV 4150

static i2c_master_dev_handle_t s_battery_dev;
static bool s_battery_ready;

static bool device_bus__battery_ensure_device(i2c_master_bus_handle_t bus) {
    if (s_battery_ready) return true;
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_BATTERY_AXP192_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_config, &s_battery_dev) != ESP_OK) return false;

    uint8_t enable_adc[] = {DEVICE_BUS__AXP192_REG_ADC_ENABLE, DEVICE_BUS__AXP192_ADC_ENABLE_ALL};
    if (i2c_master_transmit(s_battery_dev, enable_adc, sizeof(enable_adc), -1) != ESP_OK) return false;

    s_battery_ready = true;
    return true;
}

static bool device_bus__battery_read(int *out_percent) {
    uint8_t reg = DEVICE_BUS__AXP192_REG_BATTERY_VOLTAGE;
    uint8_t raw[2] = {0};
    if (i2c_master_transmit_receive(s_battery_dev, &reg, 1, raw, sizeof(raw), -1) != ESP_OK) return false;

    int adc12 = ((int)raw[0] << 4) | (raw[1] & 0x0F);
    int battery_mv = adc12 * DEVICE_BUS__AXP192_VOLTAGE_LSB_MV_NUM / DEVICE_BUS__AXP192_VOLTAGE_LSB_MV_DEN;
    int percent = (battery_mv - DEVICE_BUS__BATTERY_EMPTY_MV) * 100 /
                  (DEVICE_BUS__BATTERY_FULL_MV - DEVICE_BUS__BATTERY_EMPTY_MV);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    *out_percent = percent;
    return true;
}

#elif defined(DEVICE_BUS__BATTERY_BACKEND_BQ27220)

#define DEVICE_BUS__BQ27220_REG_STATE_OF_CHARGE 0x2C

static i2c_master_dev_handle_t s_battery_dev;
static bool s_battery_ready;

static bool device_bus__battery_ensure_device(i2c_master_bus_handle_t bus) {
    if (s_battery_ready) return true;
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_BATTERY_BQ27220_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_config, &s_battery_dev) != ESP_OK) return false;
    s_battery_ready = true;
    return true;
}

static bool device_bus__battery_read(int *out_percent) {
    uint8_t reg = DEVICE_BUS__BQ27220_REG_STATE_OF_CHARGE;
    uint8_t raw[2] = {0};
    if (i2c_master_transmit_receive(s_battery_dev, &reg, 1, raw, sizeof(raw), -1) != ESP_OK) return false;
    int percent = (int)raw[0] | ((int)raw[1] << 8);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    *out_percent = percent;
    return true;
}

#endif

#if !defined(DEVICE_BUS__NO_I2C_BATTERY)
static bool device_bus__battery_poll(i2c_master_bus_handle_t bus) {
    int percent = 0;
    if (!device_bus__battery_ensure_device(bus) || !device_bus__battery_read(&percent)) return false;
    device__set_battery_internal(percent);
    bruce_device_battery_message_t message = {.percent = percent};
    (void)pubsub__publish(BRUCE_DEVICE_TOPIC_BATTERY, &message, sizeof(message));
    return true;
}
#endif

int device_bus_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        stdio__printf("Inspect device buses.\n");
        return BRUCE_OK;
    }

#if !CONFIG_BRUCE_TOUCH_ENABLED && defined(DEVICE_BUS__NO_I2C_BATTERY)
    return BRUCE_OK;
#else
    i2c_master_bus_handle_t bus = board_i2c__acquire();
    if (bus == NULL) return BRUCE_ERR_IO;

#if CONFIG_BRUCE_TOUCH_ENABLED
    device_bus__touch_reset();
#endif

    /* The first reading at once, so the level shows right after boot; a failed one is tried again soon. */
    uint64_t next_battery_poll_at = 0;
    while (process__current_signal() == 0) {
#if CONFIG_BRUCE_TOUCH_ENABLED
        device_bus__touch_poll(bus);
#endif
#if !defined(DEVICE_BUS__NO_I2C_BATTERY)
        uint64_t now = runtime__now();
        if (now >= next_battery_poll_at) {
            bool read = device_bus__battery_poll(bus);
            next_battery_poll_at = now + (read ? DEVICE_BUS__BATTERY_POLL_PERIOD_MS : DEVICE_BUS__BATTERY_RETRY_MS);
        }
#endif
        bruce_result_t result = runtime__sleep(DEVICE_BUS__POLL_PERIOD_MS);
        if (result != BRUCE_OK && process__current_signal() == 0) break;
    }
    return BRUCE_OK;
#endif
}

#else /* !CONFIG_BRUCE_BOARD_I2C_ENABLED */

int device_bus_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return BRUCE_OK;
}

#endif
