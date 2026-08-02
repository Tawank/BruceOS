#include "display_driver.h"

#include "display_internal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h" // IWYU pragma: keep
#include "esp_log.h"

#define TAG "bruce_display"

#if defined(CONFIG_BRUCE_BOARD_M5_CARDPUTER)
#define DISPLAY__SPI_HOST SPI2_HOST
#define DISPLAY__PIN_MOSI GPIO_NUM_35
#define DISPLAY__PIN_SCK GPIO_NUM_36
#define DISPLAY__PIN_CS GPIO_NUM_37
#define DISPLAY__PIN_DC GPIO_NUM_34
#define DISPLAY__PIN_RST GPIO_NUM_33
#define DISPLAY__PIN_BL GPIO_NUM_38
#elif defined(CONFIG_BRUCE_BOARD_M5_STICKC_PLUS2)
#define DISPLAY__SPI_HOST SPI2_HOST
#define DISPLAY__PIN_MOSI GPIO_NUM_15
#define DISPLAY__PIN_SCK GPIO_NUM_13
#define DISPLAY__PIN_CS GPIO_NUM_5
#define DISPLAY__PIN_DC GPIO_NUM_14
#define DISPLAY__PIN_RST GPIO_NUM_12
#define DISPLAY__PIN_BL GPIO_NUM_27
#else
#error "No Bruce board selected; set CONFIG_BRUCE_BOARD_* via menuconfig or sdkconfig"
#endif

#define DISPLAY__BL_FREQ 256
#define DISPLAY__BL_LEDC_TIMER LEDC_TIMER_0
#define DISPLAY__BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define DISPLAY__BL_LEDC_CHANNEL LEDC_CHANNEL_7
#define DISPLAY__LEDC_TIMER_RESOLUTION LEDC_TIMER_13_BIT
#define DISPLAY__LEDC_MAX_DUTY 8191

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool s_spi_bus_initialized;
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
}

static bruce_result_t display_driver__panel_init(void) {
    gpio_config_t bl_gpio = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISPLAY__PIN_BL,
    };
    gpio_config(&bl_gpio);
    gpio_set_level(DISPLAY__PIN_BL, 0);

    spi_bus_config_t bus_config = {
        .sclk_io_num = DISPLAY__PIN_SCK,
        .mosi_io_num = DISPLAY__PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY__FB_SIZE + 8,
    };
    if (spi_bus_initialize(DISPLAY__SPI_HOST, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize SPI bus");
        return BRUCE_ERR_INTERNAL;
    }
    s_spi_bus_initialized = true;

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
    if (esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create ST7789 panel");
        display_driver__deinit();
        return BRUCE_ERR_INTERNAL;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, 52, 40);
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
    if (s_spi_bus_initialized) {
        spi_bus_free(DISPLAY__SPI_HOST);
        s_spi_bus_initialized = false;
    }
}

void display_driver__configure_rotation(uint8_t rotation) {
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    int x_gap = 0;
    int y_gap = 0;
    switch (rotation & 3) {
        case 0:
            x_gap = 52;
            y_gap = 40;
            break;
        case 1:
            swap_xy = true;
            mirror_x = true;
            x_gap = 40;
            y_gap = 53;
            break;
        case 2:
            mirror_x = true;
            mirror_y = true;
            x_gap = 53;
            y_gap = 40;
            break;
        case 3:
            swap_xy = true;
            mirror_y = true;
            x_gap = 40;
            y_gap = 52;
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
