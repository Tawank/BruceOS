#include "device_bus_app.h"
#include "core_sdk/result.h"

#if CONFIG_BRUCE_BOARD_I2C_ENABLED

#include <string.h>

#include "core/device/board_i2c.h"

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
    s_touch_ready = true;
    return true;
}

static bool device_bus__touch_read(int32_t *out_x, int32_t *out_y) {
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

static void device_bus__touch_publish(bruce_input_action_t action, int32_t x, int32_t y) {
    bruce_device_touch_message_t message = {.action = action, .x = x, .y = y};
    (void)pubsub__publish(BRUCE_DEVICE_TOPIC_TOUCH, &message, sizeof(message));
}

static void device_bus__touch_poll(i2c_master_bus_handle_t bus) {
    if (!device_bus__touch_ensure_device(bus)) return;

    int32_t x = 0;
    int32_t y = 0;
    bool pressed = device_bus__touch_read(&x, &y);

    if (pressed && !s_touch_prev_pressed) {
        device_bus__touch_publish(BRUCE_INPUT_PRESS, x, y);
    } else if (pressed && s_touch_prev_pressed && (x != s_touch_prev_x || y != s_touch_prev_y)) {
        device_bus__touch_publish(BRUCE_INPUT_CHANGE, x, y);
    } else if (!pressed && s_touch_prev_pressed) {
        device_bus__touch_publish(BRUCE_INPUT_RELEASE, s_touch_prev_x, s_touch_prev_y);
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
static void device_bus__battery_poll(i2c_master_bus_handle_t bus) {
    if (!device_bus__battery_ensure_device(bus)) return;
    int percent = 0;
    if (!device_bus__battery_read(&percent)) return;
    bruce_device_battery_message_t message = {.percent = percent};
    (void)pubsub__publish(BRUCE_DEVICE_TOPIC_BATTERY, &message, sizeof(message));
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

    uint64_t last_battery_poll_at = 0;
    while (process__current_signal() == 0) {
#if CONFIG_BRUCE_TOUCH_ENABLED
        device_bus__touch_poll(bus);
#endif
#if !defined(DEVICE_BUS__NO_I2C_BATTERY)
        uint64_t now = runtime__now();
        if (now - last_battery_poll_at >= DEVICE_BUS__BATTERY_POLL_PERIOD_MS) {
            device_bus__battery_poll(bus);
            last_battery_poll_at = now;
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
