#include "display.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/task/task.h"
#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/notification.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h" // IWYU pragma: export
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "bruce_display"

/* -------------------------------------------------------------------------- */
/* Board-specific pinout, derived from the M5GFX autodetection reference.     */
/* -------------------------------------------------------------------------- */

#if defined(CONFIG_BRUCE_BOARD_M5_CARDPUTER)
#define DISPLAY__SPI_HOST SPI2_HOST
#define DISPLAY__PIN_MOSI GPIO_NUM_35
#define DISPLAY__PIN_SCK GPIO_NUM_36
#define DISPLAY__PIN_CS GPIO_NUM_37
#define DISPLAY__PIN_DC GPIO_NUM_34
#define DISPLAY__PIN_RST GPIO_NUM_33
#define DISPLAY__PIN_BL GPIO_NUM_38
#define DISPLAY__DEFAULT_ROTATION 1
#define DISPLAY__BL_FREQ 256
#define DISPLAY__BL_LEDC_TIMER LEDC_TIMER_0
#define DISPLAY__BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define DISPLAY__BL_LEDC_CHANNEL LEDC_CHANNEL_7
#elif defined(CONFIG_BRUCE_BOARD_M5_STICKC_PLUS2)
#define DISPLAY__SPI_HOST SPI2_HOST
#define DISPLAY__PIN_MOSI GPIO_NUM_15
#define DISPLAY__PIN_SCK GPIO_NUM_13
#define DISPLAY__PIN_CS GPIO_NUM_5
#define DISPLAY__PIN_DC GPIO_NUM_14
#define DISPLAY__PIN_RST GPIO_NUM_12
#define DISPLAY__PIN_BL GPIO_NUM_27
#define DISPLAY__DEFAULT_ROTATION 3
#define DISPLAY__BL_FREQ 256
#define DISPLAY__BL_LEDC_TIMER LEDC_TIMER_0
#define DISPLAY__BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define DISPLAY__BL_LEDC_CHANNEL LEDC_CHANNEL_7
#else
#error "No Bruce board selected; set CONFIG_BRUCE_BOARD_* via menuconfig or sdkconfig"
#endif

#define DISPLAY__NATIVE_WIDTH 135
#define DISPLAY__NATIVE_HEIGHT 240
#define DISPLAY__FB_SIZE (DISPLAY__NATIVE_WIDTH * DISPLAY__NATIVE_HEIGHT * sizeof(bruce_display_color_t))
#define DISPLAY__MAX_CONTEXTS 16
#define DISPLAY__WORKER_STACK 4096
#define DISPLAY__ROW_BUF_PIXELS                                                                              \
    (DISPLAY__NATIVE_WIDTH > DISPLAY__NATIVE_HEIGHT ? DISPLAY__NATIVE_WIDTH : DISPLAY__NATIVE_HEIGHT)

#define DISPLAY__FONT_WIDTH 5
#define DISPLAY__FONT_HEIGHT 7
#define DISPLAY__FONT_FIRST 32
#define DISPLAY__FONT_LAST 126

#define DISPLAY__LEDC_TIMER_RESOLUTION LEDC_TIMER_13_BIT
#define DISPLAY__LEDC_MAX_DUTY 8191

/* -------------------------------------------------------------------------- */
/* Standard Adafruit_GFX 5x7 ASCII font (public-domain glyph shapes).         */
/* -------------------------------------------------------------------------- */

static const uint8_t s_font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x56, 0x20, 0x50}, // '&'
    {0x00, 0x08, 0x07, 0x03, 0x00}, // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')'
    {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+'
    {0x00, 0x80, 0x70, 0x30, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x00, 0x60, 0x60, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x72, 0x49, 0x49, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x49, 0x4D, 0x33}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x31}, // '6'
    {0x41, 0x21, 0x11, 0x09, 0x07}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x46, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x00, 0x14, 0x00, 0x00}, // ':'
    {0x00, 0x40, 0x34, 0x00, 0x00}, // ';'
    {0x00, 0x08, 0x14, 0x22, 0x41}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>'
    {0x02, 0x01, 0x59, 0x09, 0x06}, // '?'
    {0x3E, 0x41, 0x5D, 0x59, 0x4E}, // '@'
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 'F'
    {0x3E, 0x41, 0x41, 0x51, 0x73}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x26, 0x49, 0x49, 0x49, 0x32}, // 'S'
    {0x03, 0x01, 0x7F, 0x01, 0x03}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x03, 0x04, 0x78, 0x04, 0x03}, // 'Y'
    {0x61, 0x59, 0x49, 0x4D, 0x43}, // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x41}, // '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // '\'
    {0x00, 0x41, 0x41, 0x41, 0x7F}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_'
    {0x00, 0x03, 0x07, 0x08, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x78, 0x40}, // 'a'
    {0x7F, 0x28, 0x44, 0x44, 0x38}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x28}, // 'c'
    {0x38, 0x44, 0x44, 0x28, 0x7F}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e'
    {0x00, 0x08, 0x7E, 0x09, 0x02}, // 'f'
    {0x18, 0xA4, 0xA4, 0x9C, 0x78}, // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 'i'
    {0x20, 0x40, 0x40, 0x3D, 0x00}, // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l'
    {0x7C, 0x04, 0x78, 0x04, 0x78}, // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0xFC, 0x18, 0x24, 0x24, 0x18}, // 'p'
    {0x18, 0x24, 0x24, 0x18, 0xFC}, // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x24}, // 's'
    {0x04, 0x04, 0x3F, 0x44, 0x24}, // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x'
    {0x4C, 0x90, 0x90, 0x90, 0x7C}, // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // '{'
    {0x00, 0x00, 0x77, 0x00, 0x00}, // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // '}'
    {0x02, 0x01, 0x02, 0x04, 0x02}, // '~'
};

/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */

static SemaphoreHandle_t s_mutex;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool s_initialized;

static bruce_display_color_t *s_framebuffer;
static bruce_display_color_t s_row_buffer[DISPLAY__ROW_BUF_PIXELS];
static uint8_t s_rotation;
static int16_t s_fb_width;
static int16_t s_fb_height;
static uint8_t s_brightness;

typedef struct {
    bool in_use;
    bool gui_requested;
    bool built_in;
    bool tiled;
    bool hidden;
    bool frame_active;
    bool frame_noop;
    bool transfer_pending;
    bool remove_pending;
    bruce_task_id_t task_id;
    bruce_task_state_t state;
    bruce_display_rect_t viewport;
    uint32_t viewport_generation;
    uint32_t frame_generation;
    bruce_display_color_t text_color;
    bruce_display_color_t text_bg_color;
    bool text_bg_transparent;
    uint8_t text_size;
    int16_t cursor_x;
    int16_t cursor_y;
    SemaphoreHandle_t completion;
    bruce_result_t completion_result;
} display__task_context_t;

typedef struct {
    display__task_context_t *context;
    bruce_display_rect_t rect;
    bool fullscreen;
    bool overlay_update;
    uint32_t notification_generation;
} display__request_t;

typedef struct {
    bool active;
    char text[BRUCE_NOTIFICATION_TEXT_MAX];
    TickType_t expires_at;
    bruce_display_rect_t rect;
    uint32_t generation;
    uint32_t duration_ms;
} notification__state_t;

static display__task_context_t s_contexts[DISPLAY__MAX_CONTEXTS];
static display__task_context_t s_system_context;
static display__task_context_t *s_draw_context;
static QueueHandle_t s_request_queue;
static SemaphoreHandle_t s_transfer_done;
static TaskHandle_t s_worker_task;
static bool s_dashboard_layout;
static notification__state_t s_notification;
static bool s_transfer_active;

static bool display__on_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *event_data, void *user_ctx
);

static inline void display__lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }

static inline void display__unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static void display__ensure_lock(void) {
    if (s_mutex == NULL) { s_mutex = xSemaphoreCreateRecursiveMutex(); }
}

static bruce_display_rect_t display__fullscreen_rect(void) {
    return (bruce_display_rect_t){0, 0, s_fb_width, s_fb_height};
}

static void display__context_defaults(display__task_context_t *context) {
    context->text_color = BRUCE_COLOR_WHITE;
    context->text_bg_color = BRUCE_COLOR_BLACK;
    context->text_bg_transparent = false;
    context->text_size = 1;
    context->cursor_x = 0;
    context->cursor_y = 0;
}

static display__task_context_t *display__find_context_locked(bruce_task_id_t task_id) {
    if (task_id == BRUCE_TASK_ID_INVALID) { return &s_system_context; }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].task_id == task_id) { return &s_contexts[i]; }
    }
    return NULL;
}

static bool display__rects_overlap(bruce_display_rect_t a, bruce_display_rect_t b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static bruce_display_rect_t display__rect_union(bruce_display_rect_t a, bruce_display_rect_t b) {
    if (a.width <= 0 || a.height <= 0) return b;
    if (b.width <= 0 || b.height <= 0) return a;
    int16_t left = a.x < b.x ? a.x : b.x;
    int16_t top = a.y < b.y ? a.y : b.y;
    int16_t right_a = a.x + a.width;
    int16_t right_b = b.x + b.width;
    int16_t bottom_a = a.y + a.height;
    int16_t bottom_b = b.y + b.height;
    int16_t right = right_a > right_b ? right_a : right_b;
    int16_t bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
    return (bruce_display_rect_t){left, top, right - left, bottom - top};
}

static bruce_display_rect_t display__notification_rect(const char *text) {
    int width = (int)strlen(text) * (DISPLAY__FONT_WIDTH + 1) + 8;
    if (width > s_fb_width - 4) width = s_fb_width - 4;
    if (width < 20) width = 20;
    int height = DISPLAY__FONT_HEIGHT + 8;
    return (bruce_display_rect_t){s_fb_width - width - 2, s_fb_height - height - 2, width, height};
}

static bool display__overlay_conflicts_locked(bruce_display_rect_t rect) {
    if (s_system_context.frame_active && display__rects_overlap(s_system_context.viewport, rect)) {
        return true;
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active && !s_contexts[i].frame_noop &&
            display__rects_overlap(s_contexts[i].viewport, rect)) {
            return true;
        }
    }
    return false;
}

static void display__compose_notification_row(
    bruce_display_rect_t transfer, const notification__state_t *notification, int screen_y
) {
    bruce_display_rect_t rect = notification->rect;
    if (screen_y < rect.y || screen_y >= rect.y + rect.height) return;

    int y = screen_y;
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
        if (x < transfer.x || x >= transfer.x + transfer.width) continue;
        bool border =
            x == rect.x || y == rect.y || x == rect.x + rect.width - 1 || y == rect.y + rect.height - 1;
        s_row_buffer[(x - transfer.x)] = border ? BRUCE_COLOR_WHITE : BRUCE_COLOR_NAVY;
    }
    int text_base_y = rect.y + 4;
    if (y < text_base_y || y >= text_base_y + DISPLAY__FONT_HEIGHT) return;
    int font_row = y - text_base_y;
    int cursor_x = rect.x + 4;
    for (const char *p = notification->text; *p != '\0'; ++p) {
        if (cursor_x + DISPLAY__FONT_WIDTH >= rect.x + rect.width - 3) break;
        char c = *p;
        if (c < DISPLAY__FONT_FIRST || c > DISPLAY__FONT_LAST) {
            cursor_x += DISPLAY__FONT_WIDTH + 1;
            continue;
        }
        const uint8_t *glyph = s_font_5x7[(int)c - DISPLAY__FONT_FIRST];
        for (int col = 0; col < DISPLAY__FONT_WIDTH; ++col) {
            if (glyph[col] & (1u << font_row)) {
                int px = cursor_x + col;
                if (px >= transfer.x && px < transfer.x + transfer.width) {
                    s_row_buffer[px - transfer.x] = BRUCE_COLOR_WHITE;
                }
            }
        }
        cursor_x += DISPLAY__FONT_WIDTH + 1;
    }
}

static void display__set_visibility_locked(display__task_context_t *context) {
    bruce_display_rect_t next = {0};
    bool hidden = true;
    if (context->gui_requested && context->state == BRUCE_TASK_FOREGROUND) {
        next = display__fullscreen_rect();
        hidden = false;
    } else if (context->gui_requested && context->tiled && context->state == BRUCE_TASK_BACKGROUND) {
        next = context->viewport;
        hidden = false;
    }
    if (context->hidden != hidden || memcmp(&context->viewport, &next, sizeof(next)) != 0) {
        context->viewport = next;
        context->hidden = hidden;
        context->viewport_generation++;
    }
}

static display__task_context_t *display__drawing_context_locked(bruce_task_id_t caller) {
    display__task_context_t *context = display__find_context_locked(caller);
    if (context != NULL) { s_draw_context = context; }
    return context;
}

static bool display__on_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *event_data, void *user_ctx
) {
    (void)panel_io;
    (void)event_data;
    (void)user_ctx;
    BaseType_t high_priority_woken = pdFALSE;
    if (s_transfer_done != NULL) { xSemaphoreGiveFromISR(s_transfer_done, &high_priority_woken); }
    return high_priority_woken == pdTRUE;
}

static void display__finish_request(display__request_t *request, bruce_result_t result) {
    if (request->overlay_update) { return; }
    display__lock();
    display__task_context_t *context = request->context;
    context->completion_result = result;
    context->transfer_pending = false;
    context->frame_active = false;
    context->frame_noop = false;
    if (!context->remove_pending) { display__set_visibility_locked(context); }
    xSemaphoreGive(context->completion);
    if (context->remove_pending && context != &s_system_context) {
        context->in_use = false;
        context->remove_pending = false;
    }
    display__unlock();
}

static void display__worker(void *arg) {
    (void)arg;
    display__request_t request;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        display__lock();
        if (s_notification.active) {
            TickType_t now = xTaskGetTickCount();
            wait = (int32_t)(s_notification.expires_at - now) > 0 ? s_notification.expires_at - now : 0;
        }
        display__unlock();
        if (xQueueReceive(s_request_queue, &request, wait) != pdPASS) {
            display__lock();
            if (!s_notification.active || (int32_t)(xTaskGetTickCount() - s_notification.expires_at) < 0) {
                display__unlock();
                continue;
            }
            request = (display__request_t){
                .rect = s_notification.rect,
                .overlay_update = true,
                .notification_generation = s_notification.generation,
            };
            s_notification.active = false;
            s_notification.generation++;
            display__unlock();
        }

        display__lock();
        s_transfer_active = true;
        display__unlock();

        for (;;) {
            display__lock();
            bool conflict = request.overlay_update && display__overlay_conflicts_locked(request.rect);
            display__unlock();
            if (!conflict) { break; }
            vTaskDelay(1);
        }
        display__lock();
        notification__state_t notification = s_notification;
        bool compose = notification.active && display__rects_overlap(request.rect, notification.rect);
        bool packed = !request.fullscreen || compose || request.overlay_update;
        display__unlock();

        while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}

        if (packed) {
            int x = request.rect.x;
            int y = request.rect.y;
            int w = request.rect.width;
            int h = request.rect.height;

            for (int row = 0; row < h; ++row) {
                const bruce_display_color_t *row_pixels;
                int screen_y = y + row;
                if (compose) {
                    display__lock();
                    memcpy(
                        s_row_buffer,
                        &s_framebuffer[screen_y * s_fb_width + x],
                        (size_t)w * sizeof(bruce_display_color_t)
                    );
                    display__compose_notification_row(request.rect, &notification, screen_y);
                    display__unlock();
                    row_pixels = s_row_buffer;
                } else {
                    row_pixels = &s_framebuffer[screen_y * s_fb_width + x];
                }
                esp_err_t err =
                    esp_lcd_panel_draw_bitmap(s_panel, x, screen_y, x + w, screen_y + 1, row_pixels);
                if (err != ESP_OK || xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                    display__lock();
                    s_transfer_active = false;
                    display__unlock();
                    display__finish_request(&request, BRUCE_ERR_IO);
                    break;
                }
            }
            if (s_transfer_active) {
                s_transfer_active = false;
                display__finish_request(&request, BRUCE_OK);
            }
            continue;
        } else {
            while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
            esp_err_t err = esp_lcd_panel_draw_bitmap(
                s_panel,
                request.rect.x,
                request.rect.y,
                request.rect.x + request.rect.width,
                request.rect.y + request.rect.height,
                s_framebuffer
            );
            if (err != ESP_OK) {
                display__lock();
                s_transfer_active = false;
                display__unlock();
                display__finish_request(&request, BRUCE_ERR_IO);
                continue;
            }
            if (xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                display__lock();
                s_transfer_active = false;
                display__unlock();
                display__finish_request(&request, BRUCE_ERR_IO);
                continue;
            }
        }
        display__lock();
        s_transfer_active = false;
        display__unlock();
        display__finish_request(&request, BRUCE_OK);
    }
}

/* -------------------------------------------------------------------------- */
/* Panel rotation and pixel helpers                                           */
/* -------------------------------------------------------------------------- */

/*
 * Rotation is handled by the ST7789 panel controller via esp_lcd_panel_swap_xy()
 * and esp_lcd_panel_mirror().  The framebuffer is stored in the logical
 * orientation for the current rotation; the controller maps it to the physical
 * panel.
 */
static void display__update_fb_dimensions(void) {
    if (s_rotation == 0 || s_rotation == 2) {
        s_fb_width = DISPLAY__NATIVE_WIDTH;
        s_fb_height = DISPLAY__NATIVE_HEIGHT;
    } else {
        s_fb_width = DISPLAY__NATIVE_HEIGHT;
        s_fb_height = DISPLAY__NATIVE_WIDTH;
    }
}

static void display__configure_rotation(void) {
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    int x_gap = 0;
    int y_gap = 0;

    switch (s_rotation) {
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

    display__update_fb_dimensions();
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    esp_lcd_panel_set_gap(s_panel, x_gap, y_gap);
}

static void display__set_pixel(int16_t x, int16_t y, bruce_display_color_t color) {
    if (s_draw_context == NULL || s_draw_context->hidden || x < 0 || x >= s_draw_context->viewport.width ||
        y < 0 || y >= s_draw_context->viewport.height) {
        return;
    }
    int screen_x = s_draw_context->viewport.x + x;
    int screen_y = s_draw_context->viewport.y + y;
    if (screen_x < 0 || screen_x >= s_fb_width || screen_y < 0 || screen_y >= s_fb_height) { return; }
    s_framebuffer[screen_y * s_fb_width + screen_x] = color;
}

/* -------------------------------------------------------------------------- */
/* Backlight                                                                  */
/* -------------------------------------------------------------------------- */

static void display__set_backlight_duty(uint8_t brightness) {
    uint32_t duty = ((uint32_t)brightness * DISPLAY__LEDC_MAX_DUTY) / 255;
    ledc_set_duty(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_CHANNEL, duty);
    ledc_update_duty(DISPLAY__BL_LEDC_MODE, DISPLAY__BL_LEDC_CHANNEL);
}

static bruce_result_t display__backlight_init(void) {
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

static bruce_result_t display__panel_init(void) {
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
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY__SPI_HOST, &io_config, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create panel IO");
        return BRUCE_ERR_INTERNAL;
    }

    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = display__on_color_trans_done,
    };
    if (esp_lcd_panel_io_register_event_callbacks(s_io, &callbacks, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register panel completion callback");
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

static void
display__draw_line_bresenham(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bruce_display_color_t color) {
    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;

    for (;;) {
        display__set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) { break; }
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

static void
display__fill_rect_native(int16_t nx, int16_t ny, int16_t nw, int16_t nh, bruce_display_color_t color) {
    if (nx < 0) {
        nw += nx;
        nx = 0;
    }
    if (ny < 0) {
        nh += ny;
        ny = 0;
    }
    if (s_draw_context == NULL || s_draw_context->hidden) { return; }
    if (nx + nw > s_draw_context->viewport.width) { nw = s_draw_context->viewport.width - nx; }
    if (ny + nh > s_draw_context->viewport.height) { nh = s_draw_context->viewport.height - ny; }
    if (nw <= 0 || nh <= 0) { return; }
    nx += s_draw_context->viewport.x;
    ny += s_draw_context->viewport.y;
    for (int16_t row = ny; row < ny + nh; ++row) {
        bruce_display_color_t *row_ptr = &s_framebuffer[row * s_fb_width + nx];
        for (int16_t col = 0; col < nw; ++col) { row_ptr[col] = color; }
    }
}

static void
display__draw_circle_helper(int16_t cx, int16_t cy, int16_t r, bruce_display_color_t color, bool fill) {
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

enum {
    DISPLAY__CIRCLE_TOP_LEFT = 1 << 0,
    DISPLAY__CIRCLE_TOP_RIGHT = 1 << 1,
    DISPLAY__CIRCLE_BOTTOM_LEFT = 1 << 2,
    DISPLAY__CIRCLE_BOTTOM_RIGHT = 1 << 3,
};

static void display__draw_circle_quadrants(
    int16_t cx, int16_t cy, int16_t r, uint8_t quadrants, bruce_display_color_t color
) {
    int16_t x = 0;
    int16_t y = r;
    int16_t d = 3 - 2 * r;
    while (x <= y) {
        if (quadrants & DISPLAY__CIRCLE_TOP_LEFT) {
            display__set_pixel(cx - x, cy - y, color);
            display__set_pixel(cx - y, cy - x, color);
        }
        if (quadrants & DISPLAY__CIRCLE_TOP_RIGHT) {
            display__set_pixel(cx + x, cy - y, color);
            display__set_pixel(cx + y, cy - x, color);
        }
        if (quadrants & DISPLAY__CIRCLE_BOTTOM_LEFT) {
            display__set_pixel(cx - x, cy + y, color);
            display__set_pixel(cx - y, cy + x, color);
        }
        if (quadrants & DISPLAY__CIRCLE_BOTTOM_RIGHT) {
            display__set_pixel(cx + x, cy + y, color);
            display__set_pixel(cx + y, cy + x, color);
        }
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

static void display__draw_arc_helper(
    int16_t cx, int16_t cy, int16_t r, int start_angle, int end_angle, bruce_display_color_t color
) {
    int sweep = end_angle - start_angle;
    if (sweep == 0) { return; }
    if (sweep < 0) {
        sweep %= 360;
        sweep += 360;
    } else if (sweep > 360) {
        sweep = 360;
    }

    int normalized_start = start_angle % 360;
    if (normalized_start < 0) { normalized_start += 360; }

    int16_t previous_x = cx;
    int16_t previous_y = cy + r;
    bool have_previous = false;
    for (int offset = 0; offset <= sweep; ++offset) {
        float radians = (float)(normalized_start + offset) * ((float)M_PI / 180.0f);
        int16_t x = (int16_t)lroundf((float)cx - sinf(radians) * r);
        int16_t y = (int16_t)lroundf((float)cy + cosf(radians) * r);
        if (!have_previous) {
            display__set_pixel(x, y, color);
            have_previous = true;
        } else if (x != previous_x || y != previous_y) {
            display__draw_line_bresenham(previous_x, previous_y, x, y, color);
        }
        previous_x = x;
        previous_y = y;
    }
}

static void display__draw_triangle_fill(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
) {
    /* Sort vertices by y. */
    if (y0 > y1) {
        int16_t t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
    }
    if (y1 > y2) {
        int16_t t = x1;
        x1 = x2;
        x2 = t;
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (y0 > y1) {
        int16_t t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
    }

    int16_t dy1 = y1 - y0;
    int16_t dx1 = x1 - x0;
    int16_t dy2 = y2 - y0;
    int16_t dx2 = x2 - x0;
    int16_t dy3 = y2 - y1;
    int16_t dx3 = x2 - x1;

    for (int16_t y = y0; y <= y2; ++y) {
        if (dy1 == 0 && dy2 == 0) { break; }
        int32_t xa, xb;
        if (y <= y1 || dy1 == 0) {
            xa = dy1 == 0 ? x0 : (int32_t)x0 + (int32_t)dx1 * (y - y0) / dy1;
        } else {
            xa = dy3 == 0 ? x1 : (int32_t)x1 + (int32_t)dx3 * (y - y1) / dy3;
        }
        xb = dy2 == 0 ? x0 : (int32_t)x0 + (int32_t)dx2 * (y - y0) / dy2;
        if (xa > xb) {
            int32_t t = xa;
            xa = xb;
            xb = t;
        }
        display__draw_line_bresenham((int16_t)xa, y, (int16_t)xb, y, color);
    }
}

/* -------------------------------------------------------------------------- */
/* Text rendering                                                             */
/* -------------------------------------------------------------------------- */

static void display__draw_char(int16_t x, int16_t y, char c) {
    if (c < DISPLAY__FONT_FIRST || c > DISPLAY__FONT_LAST) { return; }
    const uint8_t *glyph = s_font_5x7[(int)c - DISPLAY__FONT_FIRST];
    int16_t width = DISPLAY__FONT_WIDTH * s_draw_context->text_size;
    int16_t height = DISPLAY__FONT_HEIGHT * s_draw_context->text_size;

    for (int16_t col = 0; col < DISPLAY__FONT_WIDTH; ++col) {
        uint8_t column = glyph[col];
        for (int16_t row = 0; row <= DISPLAY__FONT_HEIGHT; ++row) {
            if (column & (1 << row)) {
                for (int16_t dy = 0; dy < s_draw_context->text_size; ++dy) {
                    for (int16_t dx = 0; dx < s_draw_context->text_size; ++dx) {
                        display__set_pixel(
                            x + col * s_draw_context->text_size + dx,
                            y + row * s_draw_context->text_size + dy,
                            s_draw_context->text_color
                        );
                    }
                }
            } else if (!s_draw_context->text_bg_transparent) {
                for (int16_t dy = 0; dy < s_draw_context->text_size; ++dy) {
                    for (int16_t dx = 0; dx < s_draw_context->text_size; ++dx) {
                        display__set_pixel(
                            x + col * s_draw_context->text_size + dx,
                            y + row * s_draw_context->text_size + dy,
                            s_draw_context->text_bg_color
                        );
                    }
                }
            }
        }
    }

    /* Blank column between characters for readability. */
    if (!s_draw_context->text_bg_transparent) {
        for (int16_t row = 0; row <= DISPLAY__FONT_HEIGHT; ++row) {
            for (int16_t dy = 0; dy < s_draw_context->text_size; ++dy) {
                for (int16_t dx = 0; dx < s_draw_context->text_size; ++dx) {
                    display__set_pixel(
                        x + DISPLAY__FONT_WIDTH * s_draw_context->text_size + dx,
                        y + row * s_draw_context->text_size + dy,
                        s_draw_context->text_bg_color
                    );
                }
            }
        }
    }

    (void)width;
    (void)height;
}

static void display__advance_cursor(void) {
    s_draw_context->cursor_x += (DISPLAY__FONT_WIDTH + 1) * s_draw_context->text_size;
}

static void display__handle_newline(void) {
    s_draw_context->cursor_x = 0;
    s_draw_context->cursor_y += (DISPLAY__FONT_HEIGHT + 1) * s_draw_context->text_size;
}

/* -------------------------------------------------------------------------- */
/* Public API implementation                                                  */
/* -------------------------------------------------------------------------- */

bruce_result_t display__init(void) {
    display__ensure_lock();

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

    s_framebuffer =
        (bruce_display_color_t *)heap_caps_malloc(DISPLAY__FB_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
    display__configure_rotation();
    memset(&s_system_context, 0, sizeof(s_system_context));
    memset(&s_notification, 0, sizeof(s_notification));
    s_system_context.in_use = true;
    s_system_context.gui_requested = true;
    s_system_context.state = BRUCE_TASK_FOREGROUND;
    s_system_context.viewport = display__fullscreen_rect();
    s_system_context.completion = xSemaphoreCreateBinary();
    display__context_defaults(&s_system_context);

    s_transfer_done = xSemaphoreCreateBinary();
    s_request_queue = xQueueCreate(DISPLAY__MAX_CONTEXTS + 1, sizeof(display__request_t));
    if (s_system_context.completion == NULL || s_transfer_done == NULL || s_request_queue == NULL ||
        xTaskCreate(
            display__worker,
            "bruce_display",
            DISPLAY__WORKER_STACK,
            NULL,
            tskIDLE_PRIORITY + 3,
            &s_worker_task
        ) != pdPASS) {
        heap_caps_free(s_framebuffer);
        s_framebuffer = NULL;
        display__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use) { display__set_visibility_locked(&s_contexts[i]); }
    }

    memset(s_framebuffer, 0, DISPLAY__FB_SIZE);

    int cfg_bright = 100;
    if (config__get_bright(&cfg_bright) != BRUCE_OK) { cfg_bright = 100; }
    uint8_t init_brightness = (uint8_t)((cfg_bright * 255) / 100);
    display__set_backlight_duty(init_brightness);
    s_brightness = init_brightness;

    s_initialized = true;
    display__unlock();

    /* Initial flush so the black screen is visible immediately. */
    display__flush();
    return BRUCE_OK;
}

void display__deinit(void) {
    if (s_mutex == NULL) { return; }
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return;
    }

    s_initialized = false;
    display__unlock();

    /* No new requests can enter after initialized is cleared. Let the sole
     * transfer owner finish queued work before transport or DMA memory dies. */
    for (;;) {
        display__lock();
        bool idle =
            !s_transfer_active && (s_request_queue == NULL || uxQueueMessagesWaiting(s_request_queue) == 0);
        display__unlock();
        if (idle) { break; }
        vTaskDelay(1);
    }
    if (s_worker_task != NULL) {
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
    }

    display__lock();
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
    if (s_request_queue != NULL) {
        vQueueDelete(s_request_queue);
        s_request_queue = NULL;
    }
    if (s_transfer_done != NULL) {
        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;
    }

    display__unlock();
}

int display__width(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    display__task_context_t *context = display__find_context_locked(caller);
    int w = context != NULL && !context->hidden ? context->viewport.width : 0;
    display__unlock();
    return w;
}

int display__height(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    display__task_context_t *context = display__find_context_locked(caller);
    int h = context != NULL && !context->hidden ? context->viewport.height : 0;
    display__unlock();
    return h;
}

bruce_display_color_t display__color565(uint8_t r, uint8_t g, uint8_t b) {
    return (bruce_display_color_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bruce_result_t display__fill_screen(bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }

    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__fill_rect_native(0, 0, context->viewport.width, context->viewport.height, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__clear(void) { return display__fill_screen(BRUCE_COLOR_NAVY); }

bruce_result_t display__set_text_color(bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    context->text_color = color;
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_text_bg_color(uint32_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    if (color >= 0x10000) {
        context->text_bg_transparent = true;
    } else {
        context->text_bg_transparent = false;
        context->text_bg_color = (bruce_display_color_t)color;
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_text_size(uint8_t size) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    context->text_size = size < 1 ? 1 : (size > 8 ? 8 : size);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_cursor(int16_t x, int16_t y) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    context->cursor_x = x;
    context->cursor_y = y;
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__get_cursor(int16_t *x, int16_t *y) {
    if (x == NULL || y == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    *x = context->cursor_x;
    *y = context->cursor_y;
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__print(const char *text) {
    if (text == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        char c = *p;
        if (c == '\n') {
            display__handle_newline();
        } else if (c == '\r') {
            context->cursor_x = 0;
        } else if (c >= DISPLAY__FONT_FIRST && c <= DISPLAY__FONT_LAST) {
            display__draw_char(context->cursor_x, context->cursor_y, c);
            display__advance_cursor();
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__println(const char *text) {
    bruce_result_t r = display__print(text);
    if (r != BRUCE_OK) { return r; }
    return display__print("\n");
}

bruce_result_t display__draw_pixel(int16_t x, int16_t y, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__set_pixel(x, y, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t
display__draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__draw_line_bresenham(x0, y0, x1, y1, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__draw_line_bresenham(x, y, x + w - 1, y, color);
    display__draw_line_bresenham(x + w - 1, y, x + w - 1, y + h - 1, color);
    display__draw_line_bresenham(x + w - 1, y + h - 1, x, y + h - 1, color);
    display__draw_line_bresenham(x, y + h - 1, x, y, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__fill_rect_native(x, y, w, h, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__draw_circle_helper(x, y, r, color, false);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__draw_circle_helper(x, y, r, color, true);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_arc(
    int16_t x, int16_t y, int16_t r, int16_t start_angle, int16_t end_angle, bruce_display_color_t color
) {
    if (r < 0) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__draw_arc_helper(x, y, r, start_angle, end_angle, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__draw_line_bresenham(x0, y0, x1, y1, color);
    display__draw_line_bresenham(x1, y1, x2, y2, color);
    display__draw_line_bresenham(x2, y2, x0, y0, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__fill_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__draw_triangle_fill(x0, y0, x1, y1, x2, y2, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t
display__draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    int16_t max_r = (w < h ? w : h) / 2;
    if (r > max_r) { r = max_r; }
    if (r <= 0) {
        display__draw_line_bresenham(x, y, x + w - 1, y, color);
        display__draw_line_bresenham(x + w - 1, y, x + w - 1, y + h - 1, color);
        display__draw_line_bresenham(x + w - 1, y + h - 1, x, y + h - 1, color);
        display__draw_line_bresenham(x, y + h - 1, x, y, color);
        display__unlock();
        return BRUCE_OK;
    }
    display__draw_line_bresenham(x + r, y, x + w - r - 1, y, color);
    display__draw_line_bresenham(x + r, y + h - 1, x + w - r - 1, y + h - 1, color);
    display__draw_line_bresenham(x, y + r, x, y + h - r - 1, color);
    display__draw_line_bresenham(x + w - 1, y + r, x + w - 1, y + h - r - 1, color);

    display__draw_circle_quadrants(x + r, y + r, r, DISPLAY__CIRCLE_TOP_LEFT, color);
    display__draw_circle_quadrants(x + w - r - 1, y + r, r, DISPLAY__CIRCLE_TOP_RIGHT, color);
    display__draw_circle_quadrants(x + r, y + h - r - 1, r, DISPLAY__CIRCLE_BOTTOM_LEFT, color);
    display__draw_circle_quadrants(x + w - r - 1, y + h - r - 1, r, DISPLAY__CIRCLE_BOTTOM_RIGHT, color);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t
display__fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    int16_t max_r = (w < h ? w : h) / 2;
    if (r > max_r) { r = max_r; }
    display__fill_rect_native(x + r, y, w - 2 * r, h, color);
    display__fill_rect_native(x, y + r, w, h - 2 * r, color);
    display__draw_circle_helper(x + r, y + r, r, color, true);
    display__draw_circle_helper(x + w - r - 1, y + r, r, color, true);
    display__draw_circle_helper(x + r, y + h - r - 1, r, color, true);
    display__draw_circle_helper(x + w - r - 1, y + h - r - 1, r, color, true);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_bitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
) {
    if (bitmap == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    int16_t byte_width = (w + 7) / 8;
    for (int16_t row = 0; row < h; ++row) {
        for (int16_t col = 0; col < w; ++col) {
            uint8_t byte = bitmap[row * byte_width + col / 8];
            if (byte & (0x80 >> (col & 7))) {
                display__set_pixel(x + col, y + row, color);
            } else if (!context->text_bg_transparent) {
                display__set_pixel(x + col, y + row, context->text_bg_color);
            }
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_xbitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
) {
    if (bitmap == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    int16_t byte_width = (w + 7) / 8;
    for (int16_t row = 0; row < h; ++row) {
        for (int16_t col = 0; col < w; ++col) {
            uint8_t byte = bitmap[row * byte_width + col / 8];
            if (byte & (1 << (col & 7))) { display__set_pixel(x + col, y + row, color); }
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    if (bitmap == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (display__drawing_context_locked(caller) == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    for (int16_t row = 0; row < h; ++row) {
        for (int16_t col = 0; col < w; ++col) { display__set_pixel(x + col, y + row, bitmap[row * w + col]); }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_rotation(uint8_t rotation) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *caller_context = display__find_context_locked(caller);
    if (caller_context == NULL || caller_context->tiled) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    if (s_transfer_active || (s_request_queue != NULL && uxQueueMessagesWaiting(s_request_queue) != 0)) {
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active) {
            display__unlock();
            return BRUCE_ERR_BUSY;
        }
    }
    if (s_system_context.frame_active) {
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    s_rotation = rotation & 3;
    display__configure_rotation();
    if (s_notification.active) {
        s_notification.rect = display__notification_rect(s_notification.text);
        s_notification.generation++;
    }
    memset(s_framebuffer, 0, DISPLAY__FB_SIZE);
    s_system_context.viewport = display__fullscreen_rect();
    s_system_context.viewport_generation++;
    s_dashboard_layout = false;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) { continue; }
        s_contexts[i].tiled = false;
        display__set_visibility_locked(&s_contexts[i]);
    }
    display__unlock();
    return BRUCE_OK;
}

uint8_t display__get_rotation(void) {
    display__lock();
    uint8_t r = s_rotation;
    display__unlock();
    return r;
}

bruce_result_t display__invert_display(bool invert) {
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    esp_lcd_panel_invert_color(s_panel, invert);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_brightness(uint8_t brightness) {
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

uint8_t display__get_brightness(void) {
    display__lock();
    uint8_t b = s_brightness;
    display__unlock();
    return b;
}

bruce_result_t display__display_on_off(bool on) {
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    esp_lcd_panel_disp_on_off(s_panel, on);
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__begin_frame(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__find_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    if (context->completion == NULL) {
        display__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    if (context->frame_active) {
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    context->frame_active = true;
    context->frame_noop = context->hidden;
    context->frame_generation = context->viewport_generation;
    if (!context->frame_noop) {
        for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
            display__task_context_t *other = &s_contexts[i];
            if (other != context && other->in_use && other->frame_active && !other->frame_noop &&
                display__rects_overlap(context->viewport, other->viewport)) {
                context->frame_active = false;
                display__unlock();
                return BRUCE_ERR_BUSY;
            }
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__present(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__find_context_locked(caller);
    if (context == NULL || !context->frame_active) {
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    if (context->frame_noop) {
        context->frame_active = false;
        context->frame_noop = false;
        display__unlock();
        return BRUCE_OK;
    }
    if (context->frame_generation != context->viewport_generation) {
        context->frame_active = false;
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    display__request_t request = {
        .context = context,
        .rect = context->viewport,
        .fullscreen = context->viewport.x == 0 && context->viewport.y == 0 &&
                      context->viewport.width == s_fb_width && context->viewport.height == s_fb_height,
    };
    while (xSemaphoreTake(context->completion, 0) == pdTRUE) {}
    context->transfer_pending = true;
    if (xQueueSend(s_request_queue, &request, 0) != pdPASS) {
        context->transfer_pending = false;
        context->frame_active = false;
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    display__unlock();

    if (xSemaphoreTake(context->completion, portMAX_DELAY) != pdTRUE) { return BRUCE_ERR_IO; }
    return context->completion_result;
}

bruce_result_t display__flush(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    display__task_context_t *context = display__find_context_locked(caller);
    bool active = context != NULL && context->frame_active;
    display__unlock();
    if (!active) {
        bruce_result_t result = display__begin_frame();
        if (result != BRUCE_OK) { return result; }
    }
    return display__present();
}

bruce_result_t display__set_tiles(const bruce_display_tile_t *tiles, size_t count) {
    bool built_in = false;
    bruce_task_id_t caller = task__current_id();
    if ((count > 0 && tiles == NULL) || count > BRUCE_DISPLAY_MAX_TILES ||
        task_registry__current_context(&built_in, NULL, 0, NULL) != BRUCE_OK || !built_in) {
        return BRUCE_ERR_PERMISSION;
    }

    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *owner = display__find_context_locked(caller);
    if (owner == NULL || owner->state != BRUCE_TASK_FOREGROUND) {
        display__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    display__task_context_t *targets[BRUCE_DISPLAY_MAX_TILES] = {0};
    for (size_t i = 0; i < count; ++i) {
        bruce_display_rect_t rect = tiles[i].rect;
        if (rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0 ||
            rect.x + rect.width > s_fb_width || rect.y + rect.height > s_fb_height) {
            display__unlock();
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        targets[i] = display__find_context_locked(tiles[i].task_id);
        if (targets[i] == NULL || !targets[i]->gui_requested || targets[i]->state != BRUCE_TASK_BACKGROUND) {
            display__unlock();
            return BRUCE_ERR_NOT_FOUND;
        }
        for (size_t j = 0; j < i; ++j) {
            if (tiles[j].task_id == tiles[i].task_id || display__rects_overlap(tiles[j].rect, rect)) {
                display__unlock();
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
        }
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active && s_contexts[i].tiled) {
            display__unlock();
            return BRUCE_ERR_BUSY;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (targets[i]->frame_active) {
            display__unlock();
            return BRUCE_ERR_BUSY;
        }
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) { continue; }
        s_contexts[i].tiled = false;
        display__set_visibility_locked(&s_contexts[i]);
    }
    for (size_t i = 0; i < count; ++i) {
        targets[i]->tiled = true;
        targets[i]->viewport = tiles[i].rect;
        targets[i]->hidden = false;
        targets[i]->viewport_generation++;
        for (int row = 0; row < tiles[i].rect.height; ++row) {
            memset(
                &s_framebuffer[(tiles[i].rect.y + row) * s_fb_width + tiles[i].rect.x],
                0,
                (size_t)tiles[i].rect.width * sizeof(*s_framebuffer)
            );
        }
    }
    s_dashboard_layout = count > 0;
    display__unlock();
    return BRUCE_OK;
}

void display__task_created(bruce_task_id_t task_id, bool gui_requested) {
    display__ensure_lock();
    display__lock();
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) {
            SemaphoreHandle_t completion = s_contexts[i].completion;
            memset(&s_contexts[i], 0, sizeof(s_contexts[i]));
            s_contexts[i].in_use = true;
            s_contexts[i].task_id = task_id;
            s_contexts[i].gui_requested = gui_requested;
            s_contexts[i].hidden = true;
            s_contexts[i].completion = completion != NULL ? completion : xSemaphoreCreateBinary();
            display__context_defaults(&s_contexts[i]);
            break;
        }
    }
    display__unlock();
}

void display__task_state_changed(bruce_task_id_t task_id, bruce_task_state_t state) {
    display__ensure_lock();
    display__lock();
    display__task_context_t *context = display__find_context_locked(task_id);
    if (context != NULL) {
        context->state = state;
        if (!context->frame_active) { display__set_visibility_locked(context); }
    }
    display__unlock();
}

void display__task_removed(bruce_task_id_t task_id) {
    display__ensure_lock();
    display__lock();
    display__task_context_t *context = display__find_context_locked(task_id);
    if (context != NULL) {
        context->hidden = true;
        context->tiled = false;
        if (context->transfer_pending) {
            context->remove_pending = true;
        } else {
            context->frame_active = false;
            context->in_use = false;
        }
    }
    display__unlock();
}

bruce_result_t display__test_read_pixel(int16_t x, int16_t y, bruce_display_color_t *out_color) {
    if (out_color == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__lock();
    if (!s_initialized || x < 0 || y < 0 || x >= s_fb_width || y >= s_fb_height) {
        display__unlock();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_color = s_framebuffer[y * s_fb_width + x];
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__notification_push(const char *text, uint32_t duration_ms) {
    if (text == NULL || text[0] == '\0' || strlen(text) >= BRUCE_NOTIFICATION_TEXT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (duration_ms < BRUCE_NOTIFICATION_DURATION_MIN_MS) duration_ms = BRUCE_NOTIFICATION_DURATION_MIN_MS;
    if (duration_ms > BRUCE_NOTIFICATION_DURATION_MAX_MS) duration_ms = BRUCE_NOTIFICATION_DURATION_MAX_MS;

    display__ensure_lock();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    notification__state_t previous = s_notification;
    bruce_display_rect_t old_rect = s_notification.active ? s_notification.rect : (bruce_display_rect_t){0};
    strncpy(s_notification.text, text, sizeof(s_notification.text) - 1);
    s_notification.text[sizeof(s_notification.text) - 1] = '\0';
    s_notification.active = true;
    s_notification.duration_ms = duration_ms;
    s_notification.expires_at = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    s_notification.rect = display__notification_rect(s_notification.text);
    s_notification.generation++;
    display__request_t request = {
        .rect = display__rect_union(old_rect, s_notification.rect),
        .overlay_update = true,
        .notification_generation = s_notification.generation,
    };
    if (xQueueSend(s_request_queue, &request, 0) != pdPASS) {
        s_notification = previous;
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__notification_dismiss(void) {
    display__ensure_lock();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (!s_notification.active) {
        display__unlock();
        return BRUCE_OK;
    }
    notification__state_t previous = s_notification;
    display__request_t request = {
        .rect = s_notification.rect,
        .overlay_update = true,
        .notification_generation = s_notification.generation,
    };
    s_notification.active = false;
    s_notification.generation++;
    if (xQueueSend(s_request_queue, &request, 0) != pdPASS) {
        s_notification = previous;
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__test_notification(
    char *text, size_t text_size, bool *active, uint32_t *duration_ms, bruce_display_rect_t *rect,
    uint32_t *generation
) {
    if (text == NULL || text_size == 0 || active == NULL || duration_ms == NULL || rect == NULL ||
        generation == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__lock();
    strncpy(text, s_notification.text, text_size - 1);
    text[text_size - 1] = '\0';
    *active = s_notification.active;
    *duration_ms = s_notification.duration_ms;
    *rect = s_notification.rect;
    *generation = s_notification.generation;
    display__unlock();
    return BRUCE_OK;
}
