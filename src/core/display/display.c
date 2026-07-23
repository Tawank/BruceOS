#include "display.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/result.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "bruce_display"

/* -------------------------------------------------------------------------- */
/* Board-specific pinout, derived from the M5GFX autodetection reference.     */
/* -------------------------------------------------------------------------- */

#if defined(CONFIG_BRUCE_BOARD_M5_CARDPUTER)
#    define DISPLAY__SPI_HOST SPI2_HOST
#    define DISPLAY__PIN_MOSI GPIO_NUM_35
#    define DISPLAY__PIN_SCK GPIO_NUM_36
#    define DISPLAY__PIN_CS GPIO_NUM_37
#    define DISPLAY__PIN_DC GPIO_NUM_34
#    define DISPLAY__PIN_RST GPIO_NUM_33
#    define DISPLAY__PIN_BL GPIO_NUM_38
#    define DISPLAY__DEFAULT_ROTATION 1
#    define DISPLAY__BL_FREQ 256
#    define DISPLAY__BL_LEDC_TIMER LEDC_TIMER_0
#    define DISPLAY__BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#    define DISPLAY__BL_LEDC_CHANNEL LEDC_CHANNEL_7
#elif defined(CONFIG_BRUCE_BOARD_M5_STICKC_PLUS2)
#    define DISPLAY__SPI_HOST SPI2_HOST
#    define DISPLAY__PIN_MOSI GPIO_NUM_15
#    define DISPLAY__PIN_SCK GPIO_NUM_13
#    define DISPLAY__PIN_CS GPIO_NUM_5
#    define DISPLAY__PIN_DC GPIO_NUM_14
#    define DISPLAY__PIN_RST GPIO_NUM_12
#    define DISPLAY__PIN_BL GPIO_NUM_27
#    define DISPLAY__DEFAULT_ROTATION 0
#    define DISPLAY__BL_FREQ 256
#    define DISPLAY__BL_LEDC_TIMER LEDC_TIMER_0
#    define DISPLAY__BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#    define DISPLAY__BL_LEDC_CHANNEL LEDC_CHANNEL_7
#else
#    error "No Bruce board selected; set CONFIG_BRUCE_BOARD_* via menuconfig or sdkconfig"
#endif

#define DISPLAY__NATIVE_WIDTH 135
#define DISPLAY__NATIVE_HEIGHT 240
#define DISPLAY__FB_SIZE (DISPLAY__NATIVE_WIDTH * DISPLAY__NATIVE_HEIGHT * sizeof(bruce_display_color_t))

#define DISPLAY__FONT_WIDTH 5
#define DISPLAY__FONT_HEIGHT 7
#define DISPLAY__FONT_FIRST 32
#define DISPLAY__FONT_LAST 126

#define DISPLAY__LEDC_TIMER_RESOLUTION LEDC_TIMER_13_BIT
#define DISPLAY__LEDC_MAX_DUTY 8191

/* -------------------------------------------------------------------------- */
/* Minimal 5x7 ASCII font (public-domain glyph shapes).                       */
/* -------------------------------------------------------------------------- */

static const uint8_t s_font_5x7[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, // ' '
    { 0x00, 0x00, 0x05, 0x00, 0x00 }, // '!'
    { 0x00, 0x05, 0x00, 0x05, 0x00 }, // '"'
    { 0x0A, 0x1F, 0x0A, 0x1F, 0x0A }, // '#'
    { 0x0E, 0x15, 0x0E, 0x15, 0x0C }, // '$'
    { 0x12, 0x04, 0x08, 0x11, 0x00 }, // '%'
    { 0x0C, 0x12, 0x14, 0x08, 0x14 }, // '&'
    { 0x00, 0x00, 0x05, 0x00, 0x00 }, // '\''
    { 0x02, 0x04, 0x04, 0x04, 0x02 }, // '('
    { 0x08, 0x04, 0x04, 0x04, 0x08 }, // ')'
    { 0x00, 0x0A, 0x04, 0x0A, 0x00 }, // '*'
    { 0x00, 0x04, 0x0E, 0x04, 0x00 }, // '+'
    { 0x00, 0x00, 0x08, 0x04, 0x00 }, // ','
    { 0x00, 0x00, 0x0E, 0x00, 0x00 }, // '-'
    { 0x00, 0x00, 0x06, 0x00, 0x00 }, // '.'
    { 0x00, 0x04, 0x08, 0x10, 0x00 }, // '/'
    { 0x0E, 0x11, 0x11, 0x11, 0x0E }, // '0'
    { 0x04, 0x0C, 0x04, 0x04, 0x0E }, // '1'
    { 0x0E, 0x10, 0x0E, 0x01, 0x1F }, // '2'
    { 0x0E, 0x10, 0x0C, 0x10, 0x0E }, // '3'
    { 0x11, 0x11, 0x1F, 0x10, 0x10 }, // '4'
    { 0x1F, 0x01, 0x0E, 0x10, 0x0E }, // '5'
    { 0x0E, 0x01, 0x0E, 0x11, 0x0E }, // '6'
    { 0x1F, 0x10, 0x08, 0x04, 0x02 }, // '7'
    { 0x0E, 0x11, 0x0E, 0x11, 0x0E }, // '8'
    { 0x0E, 0x11, 0x1E, 0x10, 0x0E }, // '9'
    { 0x00, 0x04, 0x00, 0x04, 0x00 }, // ':'
    { 0x00, 0x04, 0x00, 0x04, 0x02 }, // ';'
    { 0x08, 0x04, 0x02, 0x04, 0x08 }, // '<'
    { 0x00, 0x0E, 0x00, 0x0E, 0x00 }, // '='
    { 0x02, 0x04, 0x08, 0x04, 0x02 }, // '>'
    { 0x0E, 0x10, 0x0C, 0x00, 0x04 }, // '?'
    { 0x0E, 0x11, 0x15, 0x15, 0x0C }, // '@'
    { 0x0E, 0x11, 0x1F, 0x11, 0x11 }, // 'A'
    { 0x1E, 0x11, 0x1E, 0x11, 0x1E }, // 'B'
    { 0x0E, 0x11, 0x01, 0x11, 0x0E }, // 'C'
    { 0x1E, 0x11, 0x11, 0x11, 0x1E }, // 'D'
    { 0x1F, 0x01, 0x0E, 0x01, 0x1F }, // 'E'
    { 0x1F, 0x01, 0x0E, 0x01, 0x01 }, // 'F'
    { 0x0E, 0x11, 0x19, 0x11, 0x0E }, // 'G'
    { 0x11, 0x11, 0x1F, 0x11, 0x11 }, // 'H'
    { 0x0E, 0x04, 0x04, 0x04, 0x0E }, // 'I'
    { 0x10, 0x10, 0x10, 0x11, 0x0E }, // 'J'
    { 0x11, 0x09, 0x06, 0x09, 0x11 }, // 'K'
    { 0x01, 0x01, 0x01, 0x01, 0x1F }, // 'L'
    { 0x11, 0x1B, 0x15, 0x11, 0x11 }, // 'M'
    { 0x11, 0x13, 0x15, 0x19, 0x11 }, // 'N'
    { 0x0E, 0x11, 0x11, 0x11, 0x0E }, // 'O'
    { 0x1E, 0x11, 0x1E, 0x01, 0x01 }, // 'P'
    { 0x0E, 0x11, 0x15, 0x19, 0x0E }, // 'Q'
    { 0x1E, 0x11, 0x1E, 0x09, 0x11 }, // 'R'
    { 0x0E, 0x01, 0x0E, 0x10, 0x0E }, // 'S'
    { 0x1F, 0x04, 0x04, 0x04, 0x04 }, // 'T'
    { 0x11, 0x11, 0x11, 0x11, 0x0E }, // 'U'
    { 0x11, 0x11, 0x11, 0x0A, 0x04 }, // 'V'
    { 0x11, 0x11, 0x15, 0x15, 0x0A }, // 'W'
    { 0x11, 0x0A, 0x04, 0x0A, 0x11 }, // 'X'
    { 0x11, 0x0A, 0x04, 0x04, 0x04 }, // 'Y'
    { 0x1F, 0x08, 0x04, 0x02, 0x1F }, // 'Z'
    { 0x0E, 0x02, 0x02, 0x02, 0x0E }, // '['
    { 0x00, 0x10, 0x08, 0x04, 0x00 }, // '\\'
    { 0x0E, 0x08, 0x08, 0x08, 0x0E }, // ']'
    { 0x00, 0x04, 0x0A, 0x00, 0x00 }, // '^'
    { 0x00, 0x00, 0x00, 0x00, 0x1F }, // '_'
    { 0x00, 0x02, 0x04, 0x00, 0x00 }, // '`'
    { 0x00, 0x0E, 0x12, 0x12, 0x0E }, // 'a'
    { 0x01, 0x0D, 0x13, 0x11, 0x0E }, // 'b'
    { 0x00, 0x0E, 0x01, 0x01, 0x0E }, // 'c'
    { 0x10, 0x16, 0x11, 0x11, 0x1E }, // 'd'
    { 0x00, 0x0E, 0x15, 0x01, 0x0E }, // 'e'
    { 0x0C, 0x02, 0x07, 0x02, 0x02 }, // 'f'
    { 0x00, 0x1E, 0x11, 0x1E, 0x10 }, // 'g'
    { 0x01, 0x0D, 0x11, 0x11, 0x11 }, // 'h'
    { 0x00, 0x06, 0x00, 0x02, 0x06 }, // 'i'
    { 0x00, 0x18, 0x08, 0x09, 0x06 }, // 'j'
    { 0x01, 0x09, 0x07, 0x09, 0x11 }, // 'k'
    { 0x03, 0x02, 0x02, 0x02, 0x06 }, // 'l'
    { 0x00, 0x15, 0x15, 0x15, 0x11 }, // 'm'
    { 0x00, 0x0D, 0x11, 0x11, 0x11 }, // 'n'
    { 0x00, 0x0E, 0x11, 0x11, 0x0E }, // 'o'
    { 0x00, 0x0E, 0x11, 0x0F, 0x01 }, // 'p'
    { 0x00, 0x0E, 0x11, 0x1E, 0x10 }, // 'q'
    { 0x00, 0x0B, 0x0D, 0x01, 0x01 }, // 'r'
    { 0x00, 0x0E, 0x0C, 0x02, 0x0E }, // 's'
    { 0x02, 0x07, 0x02, 0x02, 0x04 }, // 't'
    { 0x00, 0x11, 0x11, 0x13, 0x0E }, // 'u'
    { 0x00, 0x11, 0x11, 0x0A, 0x04 }, // 'v'
    { 0x00, 0x11, 0x15, 0x15, 0x0A }, // 'w'
    { 0x00, 0x11, 0x0A, 0x0A, 0x11 }, // 'x'
    { 0x00, 0x11, 0x11, 0x1E, 0x10 }, // 'y'
    { 0x00, 0x1F, 0x04, 0x08, 0x1F }, // 'z'
    { 0x06, 0x02, 0x04, 0x02, 0x06 }, // '{'
    { 0x04, 0x04, 0x04, 0x04, 0x04 }, // '|'
    { 0x0C, 0x08, 0x04, 0x08, 0x0C }, // '}'
    { 0x00, 0x09, 0x0E, 0x02, 0x00 }, // '~'
};

/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool s_initialized;

static bruce_display_color_t *s_framebuffer;
static uint8_t s_rotation;
static bruce_display_color_t s_text_color;
static bruce_display_color_t s_text_bg_color;
static bool s_text_bg_transparent;
static uint8_t s_text_size;
static int16_t s_cursor_x;
static int16_t s_cursor_y;
static uint8_t s_brightness;

static inline void display__lock(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

static inline void display__unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

/* -------------------------------------------------------------------------- */
/* Panel rotation and pixel helpers                                           */
/* -------------------------------------------------------------------------- */

/*
 * Rotation is handled by the ST7789 panel controller via esp_lcd_panel_swap_xy()
 * and esp_lcd_panel_mirror().  The framebuffer is always stored in the panel's
 * native orientation, so logical and native coordinates are identical.
 */
static void display__configure_rotation(void)
{
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;

    switch (s_rotation) {
        case 0:
            break;
        case 1:
            swap_xy = true;
            mirror_x = true;
            break;
        case 2:
            mirror_x = true;
            mirror_y = true;
            break;
        case 3:
            swap_xy = true;
            mirror_y = true;
            break;
    }

    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
}

static void display__set_pixel(int16_t x, int16_t y, bruce_display_color_t color)
{
    if (x < 0 || x >= DISPLAY__NATIVE_WIDTH || y < 0 || y >= DISPLAY__NATIVE_HEIGHT) {
        return;
    }
    s_framebuffer[y * DISPLAY__NATIVE_WIDTH + x] = color;
}

/* -------------------------------------------------------------------------- */
/* Backlight                                                                  */
/* -------------------------------------------------------------------------- */

static void display__set_backlight_duty(uint8_t brightness)
{
    uint32_t duty = ((uint32_t)brightness * DISPLAY__LEDC_MAX_DUTY) / 255;
    ledc_set_duty(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_CHANNEL, duty);
    ledc_update_duty(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_CHANNEL);
}

static bruce_result_t display__backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = DISPLAY__BL_LEDC_MODE,
        .duty_resolution = DISPLAY__LEDC_TIMER_RESOLUTION,
        .timer_num = DISPLAY__BL_LEDC_TIMER,
        .freq_hz = DISPLAY__BL_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
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
        return BRUCE_ERR_INTERNAL;
    }

    return BRUCE_OK;
}

/* -------------------------------------------------------------------------- */
/* LCD panel initialization                                                   */
/* -------------------------------------------------------------------------- */

static bruce_result_t display__panel_init(void)
{
    gpio_config_t bl_gpio = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISPLAY__PIN_BL,
    };
    gpio_config(&bl_gpio);
    gpio_set_level(DISPLAY__PIN_BL, 0);

    spi_bus_config_t buscfg = {
        .sclk_io_num = DISPLAY__PIN_SCK,
        .mosi_io_num = DISPLAY__PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY__FB_SIZE + 8,
    };
    if (spi_bus_initialize(DISPLAY__SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize SPI bus");
        return BRUCE_ERR_INTERNAL;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISPLAY__PIN_DC,
        .cs_gpio_num = DISPLAY__PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY__SPI_HOST, &io_config, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create panel IO");
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
        return BRUCE_ERR_INTERNAL;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, 52, 40);
    esp_lcd_panel_invert_color(s_panel, true);
    display__configure_rotation();
    esp_lcd_panel_disp_on_off(s_panel, true);

    return BRUCE_OK;
}

/* -------------------------------------------------------------------------- */
/* Drawing primitives (logical coordinates)                                   */
/* -------------------------------------------------------------------------- */

static void display__draw_line_bresenham(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                          bruce_display_color_t color)
{
    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;

    for (;;) {
        display__set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int16_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void display__fill_rect_native(int16_t nx, int16_t ny, int16_t nw, int16_t nh,
                                       bruce_display_color_t color)
{
    if (nx < 0) {
        nw += nx;
        nx = 0;
    }
    if (ny < 0) {
        nh += ny;
        ny = 0;
    }
    if (nx + nw > DISPLAY__NATIVE_WIDTH) {
        nw = DISPLAY__NATIVE_WIDTH - nx;
    }
    if (ny + nh > DISPLAY__NATIVE_HEIGHT) {
        nh = DISPLAY__NATIVE_HEIGHT - ny;
    }
    if (nw <= 0 || nh <= 0) {
        return;
    }
    for (int16_t row = ny; row < ny + nh; ++row) {
        bruce_display_color_t *row_ptr = &s_framebuffer[row * DISPLAY__NATIVE_WIDTH + nx];
        for (int16_t col = 0; col < nw; ++col) {
            row_ptr[col] = color;
        }
    }
}

static void display__draw_circle_helper(int16_t cx, int16_t cy, int16_t r,
                                         bruce_display_color_t color, bool fill)
{
    int16_t x = 0;
    int16_t y = r;
    int16_t d = 3 - 2 * r;
    while (x <= y) {
        if (fill) {
            display__draw_line_bresenham(cx - x, cy + y, cx + x, cy + y, color);
            display__draw_line_bresenham(cx - x, cy - y, cx + x, cy - y, color);
            display__draw_line_bresenham(cx - y, cy + x, cx + y, cy + x, color);
            display__draw_line_bresenham(cx - y, cy - x, cx + y, cy - x, color);
        } else {
            display__set_pixel(cx + x, cy + y, color);
            display__set_pixel(cx - x, cy + y, color);
            display__set_pixel(cx + x, cy - y, color);
            display__set_pixel(cx - x, cy - y, color);
            display__set_pixel(cx + y, cy + x, color);
            display__set_pixel(cx - y, cy + x, color);
            display__set_pixel(cx + y, cy - x, color);
            display__set_pixel(cx - y, cy - x, color);
        }
        if (d < 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

static void display__draw_triangle_fill(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                         int16_t x2, int16_t y2, bruce_display_color_t color)
{
    /* Sort vertices by y. */
    if (y0 > y1) {
        int16_t t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    if (y1 > y2) {
        int16_t t = x1; x1 = x2; x2 = t;
        t = y1; y1 = y2; y2 = t;
    }
    if (y0 > y1) {
        int16_t t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    int16_t dy1 = y1 - y0;
    int16_t dx1 = x1 - x0;
    int16_t dy2 = y2 - y0;
    int16_t dx2 = x2 - x0;
    int16_t dy3 = y2 - y1;
    int16_t dx3 = x2 - x1;

    for (int16_t y = y0; y <= y2; ++y) {
        if (dy1 == 0 && dy2 == 0) {
            break;
        }
        int32_t xa, xb;
        if (y <= y1 || dy1 == 0) {
            xa = dy1 == 0 ? x0 : (int32_t)x0 + (int32_t)dx1 * (y - y0) / dy1;
        } else {
            xa = dy3 == 0 ? x1 : (int32_t)x1 + (int32_t)dx3 * (y - y1) / dy3;
        }
        xb = dy2 == 0 ? x0 : (int32_t)x0 + (int32_t)dx2 * (y - y0) / dy2;
        if (xa > xb) {
            int32_t t = xa; xa = xb; xb = t;
        }
        display__draw_line_bresenham((int16_t)xa, y, (int16_t)xb, y, color);
    }
}

/* -------------------------------------------------------------------------- */
/* Text rendering                                                             */
/* -------------------------------------------------------------------------- */

static void display__draw_char(int16_t x, int16_t y, char c)
{
    if (c < DISPLAY__FONT_FIRST || c > DISPLAY__FONT_LAST) {
        return;
    }
    const uint8_t *glyph = s_font_5x7[(int)c - DISPLAY__FONT_FIRST];
    int16_t width = DISPLAY__FONT_WIDTH * s_text_size;
    int16_t height = DISPLAY__FONT_HEIGHT * s_text_size;

    for (int16_t col = 0; col < DISPLAY__FONT_WIDTH; ++col) {
        uint8_t column = glyph[col];
        for (int16_t row = 0; row < DISPLAY__FONT_HEIGHT; ++row) {
            if (column & (1 << row)) {
                for (int16_t dy = 0; dy < s_text_size; ++dy) {
                    for (int16_t dx = 0; dx < s_text_size; ++dx) {
                        display__set_pixel(x + col * s_text_size + dx,
                                           y + row * s_text_size + dy,
                                           s_text_color);
                    }
                }
            } else if (!s_text_bg_transparent) {
                for (int16_t dy = 0; dy < s_text_size; ++dy) {
                    for (int16_t dx = 0; dx < s_text_size; ++dx) {
                        display__set_pixel(x + col * s_text_size + dx,
                                           y + row * s_text_size + dy,
                                           s_text_bg_color);
                    }
                }
            }
        }
    }

    /* Blank column between characters for readability. */
    if (!s_text_bg_transparent) {
        for (int16_t row = 0; row < DISPLAY__FONT_HEIGHT; ++row) {
            for (int16_t dy = 0; dy < s_text_size; ++dy) {
                for (int16_t dx = 0; dx < s_text_size; ++dx) {
                    display__set_pixel(x + DISPLAY__FONT_WIDTH * s_text_size + dx,
                                       y + row * s_text_size + dy,
                                       s_text_bg_color);
                }
            }
        }
    }

    (void)width;
    (void)height;
}

static void display__advance_cursor(void)
{
    s_cursor_x += (DISPLAY__FONT_WIDTH + 1) * s_text_size;
}

static void display__handle_newline(void)
{
    s_cursor_x = 0;
    s_cursor_y += (DISPLAY__FONT_HEIGHT + 1) * s_text_size;
}

/* -------------------------------------------------------------------------- */
/* Public API implementation                                                  */
/* -------------------------------------------------------------------------- */

bruce_result_t display__init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutex_storage);
    }

    display__lock();
    if (s_initialized) {
        display__unlock();
        return BRUCE_OK;
    }

    bruce_result_t result = display__panel_init();
    if (result != BRUCE_OK) {
        display__unlock();
        return result;
    }

    result = display__backlight_init();
    if (result != BRUCE_OK) {
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(s_io);
        spi_bus_free(DISPLAY__SPI_HOST);
        s_panel = NULL;
        s_io = NULL;
        display__unlock();
        return result;
    }

    s_framebuffer = (bruce_display_color_t *)heap_caps_malloc(
        DISPLAY__FB_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_framebuffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate framebuffer");
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(s_io);
        spi_bus_free(DISPLAY__SPI_HOST);
        s_panel = NULL;
        s_io = NULL;
        display__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    s_rotation = DISPLAY__DEFAULT_ROTATION;
    s_text_color = BRUCE_COLOR_WHITE;
    s_text_bg_color = BRUCE_COLOR_BLACK;
    s_text_bg_transparent = false;
    s_text_size = 1;
    s_cursor_x = 0;
    s_cursor_y = 0;

    memset(s_framebuffer, 0, DISPLAY__FB_SIZE);

    int cfg_bright = 100;
    if (config__get_bright(&cfg_bright) != BRUCE_OK) {
        cfg_bright = 100;
    }
    uint8_t init_brightness = (uint8_t)((cfg_bright * 255) / 100);
    display__set_backlight_duty(init_brightness);
    s_brightness = init_brightness;

    s_initialized = true;
    display__unlock();

    /* Initial flush so the black screen is visible immediately. */
    display__flush();
    return BRUCE_OK;
}

void display__deinit(void)
{
    if (s_mutex == NULL) {
        return;
    }
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return;
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
    spi_bus_free(DISPLAY__SPI_HOST);

    if (s_framebuffer != NULL) {
        heap_caps_free(s_framebuffer);
        s_framebuffer = NULL;
    }

    s_initialized = false;
    display__unlock();
}

int display__width(void)
{
    display__lock();
    int w = DISPLAY__NATIVE_WIDTH;
    display__unlock();
    return w;
}

int display__height(void)
{
    display__lock();
    int h = DISPLAY__NATIVE_HEIGHT;
    display__unlock();
    return h;
}

bruce_display_color_t display__color565(uint8_t r, uint8_t g, uint8_t b)
{
    return (bruce_display_color_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bruce_result_t display__fill_screen(bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    for (size_t i = 0; i < DISPLAY__NATIVE_WIDTH * DISPLAY__NATIVE_HEIGHT; ++i) {
        s_framebuffer[i] = color;
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__clear(void)
{
    return display__fill_screen(BRUCE_COLOR_BLACK);
}

bruce_result_t display__set_text_color(bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    s_text_color = color;
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_text_bg_color(uint32_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (color >= 0x10000) {
        s_text_bg_transparent = true;
    } else {
        s_text_bg_transparent = false;
        s_text_bg_color = (bruce_display_color_t)color;
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_text_size(uint8_t size)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    s_text_size = size < 1 ? 1 : (size > 8 ? 8 : size);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_cursor(int16_t x, int16_t y)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    s_cursor_x = x;
    s_cursor_y = y;
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__get_cursor(int16_t *x, int16_t *y)
{
    if (x == NULL || y == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    *x = s_cursor_x;
    *y = s_cursor_y;
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__print(const char *text)
{
    if (text == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        char c = *p;
        if (c == '\n') {
            display__handle_newline();
        } else if (c == '\r') {
            s_cursor_x = 0;
        } else if (c >= DISPLAY__FONT_FIRST && c <= DISPLAY__FONT_LAST) {
            display__draw_char(s_cursor_x, s_cursor_y, c);
            display__advance_cursor();
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__println(const char *text)
{
    bruce_result_t r = display__print(text);
    if (r != BRUCE_OK) {
        return r;
    }
    return display__print("\n");
}

bruce_result_t display__draw_pixel(int16_t x, int16_t y, bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__set_pixel(x, y, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                   bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__draw_line_bresenham(x0, y0, x1, y1, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                   bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__draw_line_bresenham(x, y, x + w - 1, y, color);
    display__draw_line_bresenham(x + w - 1, y, x + w - 1, y + h - 1, color);
    display__draw_line_bresenham(x + w - 1, y + h - 1, x, y + h - 1, color);
    display__draw_line_bresenham(x, y + h - 1, x, y, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                   bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__fill_rect_native(x, y, w, h, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_circle(int16_t x, int16_t y, int16_t r,
                                     bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__draw_circle_helper(x, y, r, color, false);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r,
                                     bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__draw_circle_helper(x, y, r, color, true);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                       int16_t x2, int16_t y2, bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__draw_line_bresenham(x0, y0, x1, y1, color);
    display__draw_line_bresenham(x1, y1, x2, y2, color);
    display__draw_line_bresenham(x2, y2, x0, y0, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__fill_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                       int16_t x2, int16_t y2, bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__draw_triangle_fill(x0, y0, x1, y1, x2, y2, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                         int16_t r, bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    int16_t max_r = (w < h ? w : h) / 2;
    if (r > max_r) {
        r = max_r;
    }
    display__draw_line_bresenham(x + r, y, x + w - r - 1, y, color);
    display__draw_line_bresenham(x + r, y + h - 1, x + w - r - 1, y + h - 1, color);
    display__draw_line_bresenham(x, y + r, x, y + h - r - 1, color);
    display__draw_line_bresenham(x + w - 1, y + r, x + w - 1, y + h - r - 1, color);

    /* Four corner arcs approximated by small circles. */
    display__draw_circle_helper(x + r, y + r, r, color, false);
    display__draw_circle_helper(x + w - r - 1, y + r, r, color, false);
    display__draw_circle_helper(x + r, y + h - r - 1, r, color, false);
    display__draw_circle_helper(x + w - r - 1, y + h - r - 1, r, color, false);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                         int16_t r, bruce_display_color_t color)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    int16_t max_r = (w < h ? w : h) / 2;
    if (r > max_r) {
        r = max_r;
    }
    display__fill_rect(x + r, y, w - 2 * r, h, color);
    display__fill_rect(x, y + r, w, h - 2 * r, color);
    display__fill_circle(x + r, y + r, r, color);
    display__fill_circle(x + w - r - 1, y + r, r, color);
    display__fill_circle(x + r, y + h - r - 1, r, color);
    display__fill_circle(x + w - r - 1, y + h - r - 1, r, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_bitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                                     int16_t w, int16_t h, bruce_display_color_t color)
{
    if (bitmap == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    int16_t byte_width = (w + 7) / 8;
    for (int16_t row = 0; row < h; ++row) {
        for (int16_t col = 0; col < w; ++col) {
            uint8_t byte = bitmap[row * byte_width + col / 8];
            if (byte & (0x80 >> (col & 7))) {
                display__set_pixel(x + col, y + row, color);
            } else if (!s_text_bg_transparent) {
                display__set_pixel(x + col, y + row, s_text_bg_color);
            }
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_xbitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                                      int16_t w, int16_t h, bruce_display_color_t color)
{
    if (bitmap == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    int16_t byte_width = (w + 7) / 8;
    for (int16_t row = 0; row < h; ++row) {
        for (int16_t col = 0; col < w; ++col) {
            uint8_t byte = bitmap[row * byte_width + col / 8];
            if (byte & (1 << (col & 7))) {
                display__set_pixel(x + col, y + row, color);
            }
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap,
                                         int16_t w, int16_t h)
{
    if (bitmap == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    for (int16_t row = 0; row < h; ++row) {
        for (int16_t col = 0; col < w; ++col) {
            display__set_pixel(x + col, y + row, bitmap[row * w + col]);
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_rotation(uint8_t rotation)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    s_rotation = rotation & 3;
    display__configure_rotation();
    display__unlock();
    return BRUCE_OK;
}

uint8_t display__get_rotation(void)
{
    display__lock();
    uint8_t r = s_rotation;
    display__unlock();
    return r;
}

bruce_result_t display__invert_display(bool invert)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    esp_lcd_panel_invert_color(s_panel, invert);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_brightness(uint8_t brightness)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    bruce_result_t result = config__set_bright((int)brightness * 100 / 255);
    if (result == BRUCE_OK) {
        s_brightness = brightness;
        display__set_backlight_duty(brightness);
    }
    display__unlock();
    return result;
}

uint8_t display__get_brightness(void)
{
    display__lock();
    uint8_t b = s_brightness;
    display__unlock();
    return b;
}

bruce_result_t display__display_on_off(bool on)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    esp_lcd_panel_disp_on_off(s_panel, on);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__flush(void)
{
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY__NATIVE_WIDTH,
                                               DISPLAY__NATIVE_HEIGHT, s_framebuffer);
    display__unlock();
    return err == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
}
