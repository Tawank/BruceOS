#include "display_driver.h"

#include "display_internal.h"

#include "core/device/board_i2c.h" // IWYU pragma: keep

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_ili9341.h" // IWYU pragma: keep
#include "esp_lcd_io_i80.h"  // IWYU pragma: keep
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h" // IWYU pragma: keep
#include "esp_log.h"

#include <string.h>

#define TAG "bruce_display"

#define DISPLAY__PIN_CS ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_CS)
#define DISPLAY__PIN_DC ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_DC)
#define DISPLAY__PIN_RST ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_RST)
#define DISPLAY__PIN_BL ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_BL)

#if CONFIG_BRUCE_DISPLAY_BUS_SPI
#define DISPLAY__SPI_HOST ((spi_host_device_t)CONFIG_BRUCE_DISPLAY_SPI_HOST)
#define DISPLAY__PIN_MOSI ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_MOSI)
#define DISPLAY__PIN_SCK ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_SCK)
#endif

#define DISPLAY__BL_FREQ 256
#define DISPLAY__BL_LEDC_TIMER LEDC_TIMER_0
#define DISPLAY__BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define DISPLAY__BL_LEDC_CHANNEL LEDC_CHANNEL_7
#define DISPLAY__LEDC_TIMER_RESOLUTION LEDC_TIMER_13_BIT
#define DISPLAY__LEDC_MAX_DUTY 8191

/* AW9523 register map (see src/Kconfig.projbuild's
 * BRUCE_DISPLAY_BACKLIGHT_I2C_EXPANDER_ENABLED help text): 0x05 is the P1
 * direction/config register (0 = output), 0x03 is the P1 output register. */
#define DISPLAY__AW9523_REG_P1_CONFIG 0x05
#define DISPLAY__AW9523_REG_P1_OUTPUT 0x03

/* AXP192 LDO2 register map (see M5Unified's AXP192_Class.cpp): 0x28's upper
 * nibble is LDO2's voltage code ((mv-1800)/100, so 0xF = 3300mV), 0x12 bit 2
 * enables/disables LDO2. */
#define DISPLAY__AXP192_REG_LDO2_VOLTAGE 0x28
#define DISPLAY__AXP192_LDO2_VOLTAGE_3300MV_CODE 0xF0
#define DISPLAY__AXP192_REG_POWER_ENABLE 0x12
#define DISPLAY__AXP192_LDO2_ENABLE_BIT 0x04

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool s_spi_bus_initialized;
#if CONFIG_BRUCE_DISPLAY_BUS_I80
static esp_lcd_i80_bus_handle_t s_i80_bus;
#endif
static bool s_backlight_initialized;

static bool display_driver__on_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *event_data, void *user_ctx
) {
    (void)panel_io;
    (void)event_data;
    (void)user_ctx;
    return display_internal__on_transfer_done_from_isr();
}

static bruce_result_t display_driver__backlight_init(void) {
#if CONFIG_BRUCE_DISPLAY_BACKLIGHT_I2C_EXPANDER_ENABLED
    /* Some boards (e.g. M5Stack CoreS3) gate their backlight boost converter
     * through an AW9523 I2C GPIO expander instead of a GPIO Bruce can drive
     * directly. This is a one-time enable, not a dimmer - the backlight is
     * either fully on or fully off from here on. */
    i2c_master_bus_handle_t bus = board_i2c__acquire();
    if (bus == NULL) return BRUCE_ERR_INTERNAL;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_DISPLAY_BACKLIGHT_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_config, &dev) != ESP_OK) return BRUCE_ERR_INTERNAL;

    uint8_t bit = 1u << CONFIG_BRUCE_DISPLAY_BACKLIGHT_I2C_BIT;
    uint8_t config_p1[] = {DISPLAY__AW9523_REG_P1_CONFIG, (uint8_t)~bit};
    uint8_t output_p1[] = {DISPLAY__AW9523_REG_P1_OUTPUT, bit};
    bool ok = i2c_master_transmit(dev, config_p1, sizeof(config_p1), -1) == ESP_OK &&
              i2c_master_transmit(dev, output_p1, sizeof(output_p1), -1) == ESP_OK;
    (void)i2c_master_bus_rm_device(dev);
    if (!ok) {
        ESP_LOGE(TAG, "failed to enable backlight via I2C GPIO expander");
        return BRUCE_ERR_INTERNAL;
    }
    return BRUCE_OK;
#elif CONFIG_BRUCE_DISPLAY_BACKLIGHT_AXP192_LDO2_ENABLED
    /* Some boards (e.g. M5Stack Core2) power their backlight from the
     * AXP192 PMIC's LDO2 rail instead of a GPIO. One-time 3.3V enable, not
     * a dimmer - LDO2 also commonly feeds the LCD/SD logic rail itself, not
     * just the backlight, on these boards. */
    i2c_master_bus_handle_t bus = board_i2c__acquire();
    if (bus == NULL) return BRUCE_ERR_INTERNAL;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_DISPLAY_BACKLIGHT_AXP192_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_config, &dev) != ESP_OK) return BRUCE_ERR_INTERNAL;

    uint8_t read_reg = DISPLAY__AXP192_REG_LDO2_VOLTAGE;
    uint8_t ldo_reg = 0;
    bool ok = i2c_master_transmit_receive(dev, &read_reg, 1, &ldo_reg, 1, -1) == ESP_OK;
    if (ok) {
        uint8_t new_ldo_reg = (uint8_t)((ldo_reg & 0x0F) | DISPLAY__AXP192_LDO2_VOLTAGE_3300MV_CODE);
        uint8_t write_voltage[] = {DISPLAY__AXP192_REG_LDO2_VOLTAGE, new_ldo_reg};
        ok = i2c_master_transmit(dev, write_voltage, sizeof(write_voltage), -1) == ESP_OK;
    }
    uint8_t read_power = DISPLAY__AXP192_REG_POWER_ENABLE;
    uint8_t power_reg = 0;
    if (ok) ok = i2c_master_transmit_receive(dev, &read_power, 1, &power_reg, 1, -1) == ESP_OK;
    if (ok) {
        uint8_t new_power_reg = power_reg | DISPLAY__AXP192_LDO2_ENABLE_BIT;
        uint8_t write_power[] = {DISPLAY__AXP192_REG_POWER_ENABLE, new_power_reg};
        ok = i2c_master_transmit(dev, write_power, sizeof(write_power), -1) == ESP_OK;
    }
    (void)i2c_master_bus_rm_device(dev);
    if (!ok) {
        ESP_LOGE(TAG, "failed to enable backlight via AXP192 LDO2");
        return BRUCE_ERR_INTERNAL;
    }
    return BRUCE_OK;
#else
    if (DISPLAY__PIN_BL == (gpio_num_t)-1) return BRUCE_OK;

    ledc_timer_config_t timer = {
        .speed_mode = DISPLAY__BL_LEDC_MODE,
        .duty_resolution = DISPLAY__LEDC_TIMER_RESOLUTION,
        .timer_num = DISPLAY__BL_LEDC_TIMER,
        .freq_hz = DISPLAY__BL_FREQ,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure LEDC timer");
        return BRUCE_ERR_INTERNAL;
    }

    ledc_channel_config_t channel = {
        .gpio_num = DISPLAY__PIN_BL,
        .speed_mode = DISPLAY__BL_LEDC_MODE,
        .channel = DISPLAY__BL_LEDC_CHANNEL,
        .timer_sel = DISPLAY__BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure LEDC channel");
        ledc_timer_rst(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_TIMER);
        return BRUCE_ERR_INTERNAL;
    }
    s_backlight_initialized = true;
    return BRUCE_OK;
#endif
}

static bruce_result_t display_driver__panel_init(void) {
#if CONFIG_BRUCE_DISPLAY_PIN_BL >= 0
    {
        gpio_config_t bl_gpio = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << DISPLAY__PIN_BL,
        };
        gpio_config(&bl_gpio);
        gpio_set_level(DISPLAY__PIN_BL, 0);
    }
#endif

#if CONFIG_BRUCE_DISPLAY_BUS_I80
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = DISPLAY__PIN_DC,
        .wr_gpio_num = (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums =
            {
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D0,
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D1,
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D2,
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D3,
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D4,
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D5,
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D6,
                             (gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_D7,
                             },
        .bus_width = 8,
        .max_transfer_bytes = DISPLAY__FB_SIZE + 8,
        .dma_burst_size = 64,
    };
    if (esp_lcd_new_i80_bus(&bus_config, &s_i80_bus) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize i80 bus");
        return BRUCE_ERR_INTERNAL;
    }

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = DISPLAY__PIN_CS,
        .pclk_hz = CONFIG_BRUCE_DISPLAY_I80_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {.dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1},
    };
    if (esp_lcd_new_panel_io_i80(s_i80_bus, &io_config, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create i80 panel IO");
        display_driver__deinit();
        return BRUCE_ERR_INTERNAL;
    }
#else
    spi_bus_config_t bus_config = {
        .sclk_io_num = DISPLAY__PIN_SCK,
        .mosi_io_num = DISPLAY__PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY__FB_SIZE + 8,
    };
    /* ESP_ERR_INVALID_STATE means some other driver (e.g. the SD card, on
     * boards that share the LCD's SPI host - see BRUCE_SD_SPI_HOST's help
     * text) already initialized this host; that's fine, we just attach as a
     * second device below. Only free it on deinit if we were the one who
     * actually created it. */
    esp_err_t bus_error = spi_bus_initialize(DISPLAY__SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (bus_error != ESP_OK && bus_error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "failed to initialize SPI bus");
        return BRUCE_ERR_INTERNAL;
    }
    s_spi_bus_initialized = bus_error == ESP_OK;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISPLAY__PIN_DC,
        .cs_gpio_num = DISPLAY__PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY__SPI_HOST, &io_config, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create panel IO");
        display_driver__deinit();
        return BRUCE_ERR_INTERNAL;
    }
#endif

    esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = display_driver__on_color_trans_done};
    if (esp_lcd_panel_io_register_event_callbacks(s_io, &callbacks, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register panel completion callback");
        display_driver__deinit();
        return BRUCE_ERR_INTERNAL;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY__PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
#if CONFIG_BRUCE_DISPLAY_PANEL_ILI9341
    if (esp_lcd_new_panel_ili9341(s_io, &panel_config, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create ILI9341 panel");
        display_driver__deinit();
        return BRUCE_ERR_INTERNAL;
    }
#else
    if (esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create ST7789 panel");
        display_driver__deinit();
        return BRUCE_ERR_INTERNAL;
    }
#endif

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(
        s_panel, CONFIG_BRUCE_DISPLAY_GAP_X_ROTATION0, CONFIG_BRUCE_DISPLAY_GAP_Y_ROTATION0
    );
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return BRUCE_OK;
}

bruce_result_t display_driver__init(void) {
    bruce_result_t result = display_driver__panel_init();
    if (result != BRUCE_OK) { return result; }
    result = display_driver__backlight_init();
    if (result != BRUCE_OK) { display_driver__deinit(); }
    return result;
}

void display_driver__deinit(void) {
    if (s_backlight_initialized) {
        ledc_stop(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_CHANNEL, 0);
        ledc_timer_rst(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_TIMER);
        s_backlight_initialized = false;
    }
    if (s_panel != NULL) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io != NULL) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
#if CONFIG_BRUCE_DISPLAY_BUS_I80
    if (s_i80_bus != NULL) {
        esp_lcd_del_i80_bus(s_i80_bus);
        s_i80_bus = NULL;
    }
#else
    if (s_spi_bus_initialized) {
        spi_bus_free(DISPLAY__SPI_HOST);
        s_spi_bus_initialized = false;
    }
#endif
}

void display_driver__configure_rotation(uint8_t rotation) {
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    int x_gap = 0;
    int y_gap = 0;
    switch (rotation & 3) {
        case 0:
            x_gap = CONFIG_BRUCE_DISPLAY_GAP_X_ROTATION0;
            y_gap = CONFIG_BRUCE_DISPLAY_GAP_Y_ROTATION0;
            break;
        case 1:
            swap_xy = true;
            mirror_x = true;
            x_gap = CONFIG_BRUCE_DISPLAY_GAP_X_ROTATION1;
            y_gap = CONFIG_BRUCE_DISPLAY_GAP_Y_ROTATION1;
            break;
        case 2:
            mirror_x = true;
            mirror_y = true;
            x_gap = CONFIG_BRUCE_DISPLAY_GAP_X_ROTATION2;
            y_gap = CONFIG_BRUCE_DISPLAY_GAP_Y_ROTATION2;
            break;
        case 3:
            swap_xy = true;
            mirror_y = true;
            x_gap = CONFIG_BRUCE_DISPLAY_GAP_X_ROTATION3;
            y_gap = CONFIG_BRUCE_DISPLAY_GAP_Y_ROTATION3;
            break;
    }
    if (s_panel != NULL) {
        esp_lcd_panel_swap_xy(s_panel, swap_xy);
        esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
        esp_lcd_panel_set_gap(s_panel, x_gap, y_gap);
    }
}

bruce_result_t display_driver__draw_bitmap(
    int x_start, int y_start, int x_end, int y_end, const bruce_display_color_t *pixels
) {
    return esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, pixels) == ESP_OK
               ? BRUCE_OK
               : BRUCE_ERR_IO;
}

void display_driver__set_backlight(uint8_t brightness) {
    if (!s_backlight_initialized) return;
    uint32_t duty = ((uint32_t)brightness * DISPLAY__LEDC_MAX_DUTY) / 255;
    ledc_set_duty(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_CHANNEL, duty);
    ledc_update_duty(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_CHANNEL);
}

bruce_result_t display_driver__invert(bool invert) {
    return esp_lcd_panel_invert_color(s_panel, invert) == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t display_driver__set_enabled(bool enabled) {
    return esp_lcd_panel_disp_on_off(s_panel, enabled) == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
}
