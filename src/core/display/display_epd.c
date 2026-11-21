#include "display_epd.h"

#include "sdkconfig.h"

#if CONFIG_BRUCE_DISPLAY_PANEL_UC8253

#include <stdint.h>
#include <string.h>

#include "display_internal.h"

#include "core_sdk/config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_private/spi_common_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "bruce_epd"

#define EPD__WIDTH CONFIG_BRUCE_DISPLAY_WIDTH
#define EPD__HEIGHT CONFIG_BRUCE_DISPLAY_HEIGHT
#define EPD__BYTES (EPD__WIDTH * EPD__HEIGHT / 8)
#define EPD__HOST ((spi_host_device_t)CONFIG_BRUCE_DISPLAY_SPI_HOST)
#define EPD__PIN_DC ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_DC)
#define EPD__PIN_RST ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_RST)
#define EPD__PIN_BUSY ((gpio_num_t)CONFIG_BRUCE_DISPLAY_PIN_BUSY)
#define EPD__SPI_HZ 4000000
#define EPD__CHUNK 4096        /* The SD card's bus allows no bigger transfers. */
#define EPD__SETTLE_MS 40      /* Wait for drawing to stop before refreshing. */
#define EPD__SETTLE_MAX 6      /* ...but refresh anyway during animations. */
#define EPD__BUSY_TIMEOUT_MS 4000
#define EPD__ROW (EPD__WIDTH / 8)
/* The partial window's x goes to the panel as one byte. */
_Static_assert(EPD__WIDTH % 8 == 0 && EPD__WIDTH <= 256, "e-paper width must fit the partial window");
#define EPD__FULL_IDLE_MS 2000 /* A due full refresh waits for this long without drawing. */

#if CONFIG_BRUCE_SD_ENABLED && CONFIG_BRUCE_SD_SPI_HOST == CONFIG_BRUCE_DISPLAY_SPI_HOST
#define EPD__PIN_MISO CONFIG_BRUCE_SD_PIN_MISO
#else
#define EPD__PIN_MISO -1
#endif

/* One bit per pixel, 8 pixels across per byte, 1 = white. */
static uint8_t *s_drawn;   /* What apps have drawn. */
static uint8_t *s_sending; /* Snapshot being sent. */
static uint8_t *s_shown;   /* What the panel shows. */
static uint8_t *s_chunk;   /* Internal DMA copy of each SPI write; the frames above live in PSRAM. */
static uint8_t *s_window;  /* One frame's partial window, row after row. */
/* Partial refreshes each byte (8 pixels) changed in since the last full
 * refresh: ghosting builds up where the panel changes, not with the count of
 * refreshes. */
static uint8_t *s_wear;
static int s_wear_max;
static SemaphoreHandle_t s_drawn_lock;
static SemaphoreHandle_t s_panel_lock;
static TaskHandle_t s_task;
static spi_device_handle_t s_spi;
static bool s_bus_owned;
static uint8_t s_rotation;
static bool s_invert;
static bool s_enabled = true;
static volatile bool s_full_requested = true; /* The panel's content is unknown at start. */
static volatile bool s_frozen;                /* Shutdown/suspend: keep showing what is on the panel. */
static volatile uint32_t s_pull_seq;          /* Refresh rounds started... */
static volatile uint32_t s_done_seq;          /* ...and the last one finished. */

static void display_epd__send(bool data, const uint8_t *bytes, size_t len, bool invert) {
    gpio_set_level(EPD__PIN_DC, data);
    while (len > 0) {
        size_t n = len < EPD__CHUNK ? len : EPD__CHUNK;
        memcpy(s_chunk, bytes, n);
        for (size_t i = 0; invert && i < n; ++i) s_chunk[i] = (uint8_t)~s_chunk[i];
        spi_transaction_t t = {.length = n * 8, .tx_buffer = s_chunk};
        /* Interrupt-driven, so the task sleeps through the ~40 ms frame instead of spinning a core. */
        esp_err_t err = spi_device_transmit(s_spi, &t);
        if (err != ESP_OK) ESP_LOGW(TAG, "SPI write of %u bytes failed: %s", (unsigned)n, esp_err_to_name(err));
        bytes += n;
        len -= n;
    }
}

static void display_epd__cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    display_epd__send(false, &cmd, 1, false);
    if (len > 0) display_epd__send(true, data, len, false);
}

/* Returns how long the panel was busy, in ms. */
static int display_epd__wait_idle(const char *step) {
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(EPD__PIN_BUSY) == 0) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(EPD__BUSY_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "panel stayed busy after %s", step);
            break;
        }
        vTaskDelay(1);
    }
    return (int)pdTICKS_TO_MS(xTaskGetTickCount() - start);
}

/* Rows y0..y1 and bytes x0..x1 of a row, inclusive. */
typedef struct {
    int x0, x1, y0, y1;
} display_epd__rect_t;

/* Where s_sending differs from s_shown; false when nowhere. */
static bool display_epd__changed(display_epd__rect_t *out) {
    *out = (display_epd__rect_t){.x0 = EPD__ROW, .x1 = -1, .y0 = EPD__HEIGHT, .y1 = -1};
    for (int y = 0; y < EPD__HEIGHT; ++y) {
        const uint8_t *sending = s_sending + y * EPD__ROW;
        const uint8_t *shown = s_shown + y * EPD__ROW;
        if (memcmp(sending, shown, EPD__ROW) == 0) continue;
        if (out->y0 > y) out->y0 = y;
        out->y1 = y;
        for (int x = 0; x < EPD__ROW; ++x) {
            if (sending[x] == shown[x]) continue;
            if (out->x0 > x) out->x0 = x;
            if (out->x1 < x) out->x1 = x;
        }
    }
    return out->y1 >= 0;
}

/* Copies the window out of `frame` into s_window. */
static size_t display_epd__window(const uint8_t *frame, const display_epd__rect_t *window) {
    size_t width = (size_t)(window->x1 - window->x0 + 1);
    size_t n = 0;
    for (int y = window->y0; y <= window->y1; ++y) {
        memcpy(s_window + n, frame + y * EPD__ROW + window->x0, width);
        n += width;
    }
    return n;
}

/* Counts the bytes about to change in a partial refresh. */
static void display_epd__wear(const display_epd__rect_t *window) {
    for (int y = window->y0; y <= window->y1; ++y) {
        for (int x = window->x0; x <= window->x1; ++x) {
            int i = y * EPD__ROW + x;
            if (s_sending[i] == s_shown[i] || s_wear[i] == UINT8_MAX) continue;
            if (++s_wear[i] > s_wear_max) s_wear_max = s_wear[i];
        }
    }
}

/* Same order as GxEPD2's GDEQ031T10 driver, which LilyGO's firmware uses:
 * reset, panel setting, image data, then mode, power on and refresh. The
 * panel setting also holds the scan direction: it must only be sent once the
 * controller is out of reset, or that one refresh is drawn flipped. */
static void display_epd__refresh(bool full, const display_epd__rect_t *window) {
    gpio_set_level(EPD__PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD__PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    int busy_after_reset = gpio_get_level(EPD__PIN_BUSY);
    display_epd__wait_idle("reset");

    display_epd__cmd(0x00, (const uint8_t[]){0x1F, 0x0D}, 2); /* Panel setting: black/white, LUT from OTP. */
    /* KW mode picks each pixel's waveform from its old and new value, and a
     * pixel white (or black) in both gets the unchanged-pixel one (UC8253
     * datasheet, R50H DDX), which leaves partial-refresh ghosts in place. A
     * full refresh claims the old image was the inverse, so every pixel is
     * driven through the black-to-white or white-to-black waveform. */
    if (window != NULL) {
        /* Partial in: only the window is written and refreshed; the rest of
         * the panel is not driven at all (GxEPD2 GDEQ031T10, 0x90 x in pixels
         * on byte boundaries, y in two bytes each, then PT_SCAN). */
        uint16_t y0 = (uint16_t)window->y0, y1 = (uint16_t)window->y1;
        display_epd__cmd(0x91, NULL, 0);
        const uint8_t area[] = {
            (uint8_t)(window->x0 * 8),
            (uint8_t)(window->x1 * 8 + 7),
            (uint8_t)(y0 >> 8),
            (uint8_t)y0,
            (uint8_t)(y1 >> 8),
            (uint8_t)y1,
            0x01
        };
        display_epd__cmd(0x90, area, sizeof(area));
        display_epd__cmd(0x10, s_window, display_epd__window(s_shown, window));
        display_epd__cmd(0x13, s_window, display_epd__window(s_sending, window));
    } else {
        display_epd__send(false, &(const uint8_t){0x10}, 1, false);
        display_epd__send(true, full ? s_sending : s_shown, EPD__BYTES, full);
        display_epd__cmd(0x13, s_sending, EPD__BYTES);
    }

    if (!full) {
        display_epd__cmd(0xE0, (const uint8_t[]){0x02}, 1); /* Force temperature... */
        display_epd__cmd(0xE5, (const uint8_t[]){0x79}, 1); /* ...to pick the fast partial LUT. */
    }
    display_epd__cmd(0x50, (const uint8_t[]){full ? 0x97 : 0xD7}, 1);
    display_epd__cmd(0x04, NULL, 0); /* Power on. */
    int power_on_ms = display_epd__wait_idle("power on");
    display_epd__cmd(0x12, NULL, 0); /* Refresh. */
    vTaskDelay(1); /* At least 2 ms: one tick, since pdMS_TO_TICKS(2) is 0 at 100 Hz. */
    int busy_in_refresh = gpio_get_level(EPD__PIN_BUSY);
    int refresh_ms = display_epd__wait_idle("refresh");
    if (window != NULL) display_epd__cmd(0x92, NULL, 0); /* Partial out. */
    display_epd__cmd(0x02, NULL, 0); /* Power off. */
    int power_off_ms = display_epd__wait_idle("power off");
    ESP_LOGD(TAG, "%s refresh: BUSY after reset %d, power on %d ms, BUSY in refresh %d, refresh %d ms, power off %d ms",
             full ? "full" : "partial", busy_after_reset, power_on_ms, busy_in_refresh, refresh_ms, power_off_ms);
}

static bool display_epd__load_dither(void);
static void display_epd__set_dither(int mode);

static void display_epd__task(void *arg) {
    (void)arg;
    bool full_due = false;
    for (;;) {
        /* Full refreshes clear the ghosting partial ones leave; see the user
         * setting. One that is due waits for drawing to pause, so it doesn't
         * flash in the middle of use. */
        bool quiet =
            ulTaskNotifyTake(pdTRUE, full_due ? pdMS_TO_TICKS(EPD__FULL_IDLE_MS) : portMAX_DELAY) == 0;
        if (quiet) s_full_requested = true;
        for (int i = 0;
             !quiet && i < EPD__SETTLE_MAX && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EPD__SETTLE_MS));
             ++i) {}

        if (s_frozen) continue;
        uint32_t seq = ++s_pull_seq;
        display_internal__epd_pull(display_epd__load_dither());
        xSemaphoreTake(s_drawn_lock, portMAX_DELAY);
        memcpy(s_sending, s_drawn, EPD__BYTES);
        bool enabled = s_enabled;
        xSemaphoreGive(s_drawn_lock);
        bool full = s_full_requested;
        display_epd__rect_t changed;
        bool any = display_epd__changed(&changed);
        if (!enabled || (!full && !any)) {
            s_done_seq = seq;
            continue;
        }

        /* Drawing that never pauses still gets one, at twice the count. */
        int every = config__get_display_full_refresh_every();
        if (every > 0 && s_wear_max >= 2 * every) full = true;
        s_full_requested = false;
        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        if (s_frozen) {
            xSemaphoreGive(s_panel_lock);
            continue;
        }
        if (full) {
            memset(s_wear, 0, EPD__BYTES);
            s_wear_max = 0;
        } else {
            display_epd__wear(&changed);
        }
        display_epd__refresh(full, full ? NULL : &changed);
        xSemaphoreGive(s_panel_lock);
        memcpy(s_shown, s_sending, EPD__BYTES);
        full_due = every > 0 && s_wear_max >= every;
        s_done_seq = seq;
    }
}

static bruce_result_t display_epd__init_gpio(void) {
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << EPD__PIN_DC) | (1ULL << EPD__PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << EPD__PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&out) != ESP_OK || gpio_config(&in) != ESP_OK) return BRUCE_ERR_INTERNAL;
    gpio_set_level(EPD__PIN_RST, 1);
    return BRUCE_OK;
}

static bruce_result_t display_epd__init_spi(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_BRUCE_DISPLAY_PIN_SCK,
        .mosi_io_num = CONFIG_BRUCE_DISPLAY_PIN_MOSI,
        .miso_io_num = EPD__PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD__CHUNK,
    };
    /* The SD card may already own the shared bus. */
    if (spi_bus_get_attr(EPD__HOST) == NULL) {
        if (spi_bus_initialize(EPD__HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return BRUCE_ERR_INTERNAL;
        s_bus_owned = true;
    }

    spi_device_interface_config_t device = {
        .clock_speed_hz = EPD__SPI_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_BRUCE_DISPLAY_PIN_CS,
        .cs_ena_pretrans = 2,
        .cs_ena_posttrans = 2,
        .queue_size = 1,
    };
    return spi_bus_add_device(EPD__HOST, &device, &s_spi) == ESP_OK ? BRUCE_OK : BRUCE_ERR_INTERNAL;
}

static uint8_t *display_epd__alloc_frame(void) {
    uint8_t *frame = heap_caps_malloc(EPD__BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return frame != NULL ? frame : heap_caps_malloc(EPD__BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bruce_result_t display_epd__init(void) {
    s_drawn = display_epd__alloc_frame();
    s_shown = display_epd__alloc_frame();
    s_sending = display_epd__alloc_frame();
    s_window = display_epd__alloc_frame();
    s_wear = display_epd__alloc_frame();
    s_chunk = heap_caps_malloc(EPD__CHUNK, MALLOC_CAP_DMA);
    s_drawn_lock = xSemaphoreCreateMutex();
    s_panel_lock = xSemaphoreCreateMutex();
    if (s_drawn == NULL || s_shown == NULL || s_sending == NULL || s_window == NULL || s_wear == NULL ||
        s_chunk == NULL || s_drawn_lock == NULL || s_panel_lock == NULL) {
        display_epd__deinit();
        return BRUCE_ERR_NO_MEMORY;
    }
    memset(s_drawn, 0xFF, EPD__BYTES);
    display_epd__set_dither(BRUCE_DISPLAY_DITHER_BAYER4);
    memset(s_shown, 0x00, EPD__BYTES); /* Unknown: never equal to the first frame, so it is always drawn. */
    memset(s_wear, 0, EPD__BYTES);
    s_wear_max = 0;

    bruce_result_t result = display_epd__init_gpio();
    if (result == BRUCE_OK) result = display_epd__init_spi();
    if (result == BRUCE_OK && xTaskCreate(display_epd__task, "epd", 4096, NULL, 3, &s_task) != pdPASS) {
        result = BRUCE_ERR_NO_MEMORY;
    }
    if (result != BRUCE_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        display_epd__deinit();
    }
    return result;
}

void display_epd__deinit(void) {
    if (s_task != NULL) {
        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        vTaskDelete(s_task);
        s_task = NULL;
        xSemaphoreGive(s_panel_lock);
    }
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_bus_owned) {
        spi_bus_free(EPD__HOST);
        s_bus_owned = false;
    }
    if (s_drawn_lock != NULL) vSemaphoreDelete(s_drawn_lock);
    if (s_panel_lock != NULL) vSemaphoreDelete(s_panel_lock);
    s_drawn_lock = s_panel_lock = NULL;
    heap_caps_free(s_drawn);
    heap_caps_free(s_shown);
    heap_caps_free(s_sending);
    heap_caps_free(s_chunk);
    heap_caps_free(s_window);
    heap_caps_free(s_wear);
    s_drawn = s_shown = s_sending = s_chunk = s_window = s_wear = NULL;
}

void display_epd__configure_rotation(uint8_t rotation) { s_rotation = rotation & 3; }

/* Dither thresholds are laid out in panel space, so partial redraws line up with what is already
 * shown. They run 8..248: pure black and white (text, the E-paper theme) never get dots, only
 * grays and colors (games, images) turn into patterns. */
static const uint8_t s_bayer2[2][2] = {{32, 160}, {224, 96}};
static const uint8_t s_bayer4[4][4] = {
    {8, 136, 40, 168}, {200, 72, 232, 104}, {56, 184, 24, 152}, {248, 120, 216, 88}
};
/* Void-and-cluster blue noise. */
static const uint8_t s_blue_noise[32][32] = {
    {61, 90, 31, 79, 221, 16, 128, 159, 105, 13, 209, 36, 240, 129, 177, 91,
     198, 13, 133, 228, 67, 120, 17, 75, 154, 125, 8, 182, 234, 99, 73, 12},
    {169, 207, 185, 141, 43, 188, 69, 235, 48, 192, 118, 164, 52, 146, 68, 31,
     242, 168, 51, 190, 34, 141, 184, 220, 40, 247, 55, 109, 137, 42, 196, 123},
    {106, 46, 124, 236, 97, 150, 211, 88, 175, 138, 79, 221, 97, 201, 227, 122,
     152, 75, 116, 98, 157, 232, 56, 113, 92, 195, 147, 208, 82, 219, 151, 238},
    {24, 220, 71, 9, 167, 59, 20, 125, 30, 241, 60, 26, 180, 9, 107, 47,
     193, 22, 223, 204, 12, 81, 200, 135, 20, 174, 72, 30, 169, 16, 58, 85},
    {158, 138, 181, 114, 203, 231, 109, 164, 199, 100, 155, 196, 132, 82, 172, 218,
     95, 138, 170, 60, 128, 178, 37, 160, 236, 49, 120, 227, 100, 133, 187, 212},
    {101, 55, 246, 86, 35, 140, 78, 219, 65, 40, 226, 110, 53, 246, 156, 35,
     63, 233, 28, 89, 245, 104, 216, 71, 101, 209, 142, 190, 63, 240, 117, 40},
    {204, 165, 27, 131, 191, 49, 181, 24, 130, 186, 83, 143, 32, 205, 73, 126,
     188, 109, 158, 207, 41, 148, 24, 122, 184, 11, 85, 39, 161, 21, 178, 76},
    {14, 112, 214, 65, 230, 102, 148, 243, 105, 160, 18, 215, 167, 114, 15, 147,
     210, 49, 74, 132, 187, 64, 173, 232, 52, 158, 244, 111, 216, 91, 147, 229},
    {186, 143, 89, 175, 157, 11, 62, 87, 206, 53, 238, 69, 93, 192, 236, 86,
     26, 176, 239, 10, 113, 225, 93, 139, 78, 199, 131, 57, 197, 34, 128, 53},
    {69, 225, 30, 46, 116, 222, 193, 163, 26, 139, 120, 180, 43, 134, 61, 166,
     223, 120, 94, 197, 163, 51, 18, 213, 36, 101, 23, 179, 75, 159, 237, 104},
    {169, 124, 196, 242, 70, 133, 39, 107, 230, 79, 197, 8, 157, 222, 31, 104,
     141, 38, 60, 145, 77, 206, 106, 161, 184, 235, 149, 224, 119, 8, 208, 38},
    {82, 15, 96, 152, 183, 85, 214, 174, 46, 153, 99, 233, 113, 81, 206, 184,
     72, 202, 170, 227, 29, 134, 243, 70, 120, 54, 87, 41, 170, 94, 140, 193},
    {234, 136, 217, 55, 20, 145, 28, 122, 71, 218, 29, 63, 177, 48, 150, 13,
     127, 247, 20, 116, 91, 188, 41, 153, 11, 203, 137, 215, 66, 244, 52, 115},
    {64, 33, 171, 110, 234, 195, 98, 245, 186, 134, 168, 208, 121, 241, 95, 216,
     54, 102, 155, 49, 200, 62, 173, 98, 228, 178, 109, 18, 189, 154, 23, 177},
    {214, 158, 202, 72, 129, 44, 162, 59, 12, 106, 44, 85, 19, 142, 38, 163,
     192, 83, 180, 218, 123, 233, 25, 133, 76, 50, 162, 81, 129, 103, 226, 89},
    {121, 48, 92, 9, 176, 224, 82, 207, 150, 229, 191, 130, 221, 183, 74, 118,
     226, 33, 137, 9, 78, 149, 108, 185, 206, 33, 241, 217, 56, 201, 38, 145},
    {21, 247, 140, 218, 105, 29, 139, 117, 36, 92, 70, 165, 57, 103, 200, 16,
     146, 69, 238, 103, 169, 45, 214, 59, 151, 122, 96, 146, 27, 166, 77, 187},
    {66, 107, 190, 56, 152, 195, 67, 240, 161, 201, 16, 243, 32, 153, 233, 51,
     174, 111, 204, 61, 194, 245, 17, 84, 230, 22, 191, 70, 116, 239, 135, 210},
    {175, 156, 81, 37, 235, 90, 14, 180, 47, 104, 142, 119, 212, 90, 130, 80,
     217, 39, 154, 25, 129, 93, 139, 179, 106, 167, 46, 211, 180, 10, 99, 42},
    {229, 15, 203, 132, 171, 115, 215, 131, 84, 230, 185, 76, 43, 176, 19, 191,
     135, 90, 231, 182, 71, 164, 34, 210, 68, 135, 235, 83, 154, 55, 218, 125},
    {89, 115, 57, 221, 23, 62, 150, 34, 205, 58, 23, 157, 199, 110, 239, 61,
     162, 14, 117, 50, 203, 110, 225, 52, 196, 15, 100, 31, 114, 192, 73, 150},
    {35, 240, 160, 83, 190, 101, 237, 165, 111, 174, 126, 222, 63, 144, 28, 101,
     198, 220, 148, 83, 241, 10, 144, 79, 121, 158, 217, 141, 172, 244, 19, 199},
    {65, 182, 124, 43, 139, 211, 50, 80, 11, 245, 98, 35, 87, 228, 172, 124,
     41, 67, 177, 27, 126, 189, 97, 173, 237, 45, 189, 64, 37, 92, 135, 165},
    {215, 96, 8, 227, 170, 27, 127, 198, 145, 68, 209, 189, 137, 50, 207, 78,
     246, 136, 95, 209, 156, 64, 38, 213, 22, 112, 88, 229, 126, 213, 47, 108},
    {29, 143, 205, 111, 72, 93, 179, 224, 118, 45, 151, 22, 168, 112, 17, 156,
     185, 30, 225, 45, 108, 233, 162, 132, 74, 182, 155, 11, 197, 75, 179, 231},
    {58, 80, 187, 47, 242, 153, 56, 19, 88, 173, 231, 102, 73, 194, 232, 97,
     60, 115, 166, 74, 199, 19, 88, 194, 51, 246, 136, 60, 166, 119, 18, 154},
    {130, 238, 164, 123, 30, 202, 107, 188, 237, 32, 127, 54, 220, 131, 41, 146,
     213, 13, 188, 144, 128, 56, 223, 149, 98, 26, 203, 105, 40, 236, 95, 201},
    {37, 91, 14, 66, 143, 219, 77, 134, 155, 66, 186, 208, 8, 90, 181, 68,
     125, 236, 86, 36, 243, 168, 113, 34, 214, 121, 171, 80, 211, 146, 65, 175},
    {114, 212, 181, 226, 96, 169, 44, 12, 198, 94, 112, 159, 138, 244, 32, 202,
     163, 48, 105, 195, 71, 10, 184, 79, 161, 64, 228, 15, 183, 128, 23, 225},
    {54, 148, 76, 131, 25, 191, 119, 247, 53, 216, 24, 45, 77, 171, 118, 99,
     21, 223, 151, 124, 219, 94, 204, 127, 239, 42, 142, 94, 53, 242, 84, 161},
    {102, 21, 200, 49, 234, 62, 147, 84, 167, 123, 178, 229, 195, 59, 152, 232,
     75, 183, 57, 26, 159, 42, 144, 58, 25, 193, 113, 165, 206, 117, 39, 194},
    {136, 244, 160, 116, 173, 100, 205, 33, 224, 67, 143, 87, 109, 17, 212, 44,
     140, 108, 210, 86, 176, 240, 103, 210, 172, 86, 221, 68, 28, 149, 176, 222},
};
static uint8_t s_pattern[32][32];
static bool s_noise; /* White noise comes from a hash of the position, not s_pattern. */
static bool s_atkinson;

/* Atkinson error for the current row and the two below it, indexed by x - x_start + 2. Kept between
 * calls, so the chunks of one frame continue where the previous chunk stopped. */
#define EPD__SIDE (EPD__WIDTH > EPD__HEIGHT ? EPD__WIDTH : EPD__HEIGHT)
static int16_t s_error[3][EPD__SIDE + 4];
static int16_t *s_error_rows[3] = {s_error[0], s_error[1], s_error[2]};
static int s_error_y = -1, s_error_x0, s_error_x1;
static int s_dither;

/* Call with s_drawn_lock held. */
static void display_epd__set_dither(int mode) {
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            switch (mode) {
                case BRUCE_DISPLAY_DITHER_THRESHOLD: s_pattern[y][x] = 128; break;
                case BRUCE_DISPLAY_DITHER_BAYER2: s_pattern[y][x] = s_bayer2[y & 1][x & 1]; break;
                case BRUCE_DISPLAY_DITHER_BLUE_NOISE: s_pattern[y][x] = s_blue_noise[y][x]; break;
                default: s_pattern[y][x] = s_bayer4[y & 3][x & 3]; break;
            }
        }
    }
    s_noise = mode == BRUCE_DISPLAY_DITHER_NOISE;
    s_atkinson = mode == BRUCE_DISPLAY_DITHER_ATKINSON;
    s_error_y = -1;
    s_dither = mode;
}

/* Applies the dither setting; true when it changed, so the whole frame needs converting again. */
static bool display_epd__load_dither(void) {
    int mode = config__get_display_dither();
    if (mode == s_dither) return false;
    xSemaphoreTake(s_drawn_lock, portMAX_DELAY);
    display_epd__set_dither(mode);
    xSemaphoreGive(s_drawn_lock);
    return true;
}

static inline unsigned display_epd__noise(unsigned x, unsigned y) {
    unsigned h = x * 0x9E3779B1u ^ y * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    return 8 + (((h >> 16) & 0xFFu) * 240u >> 8);
}

/* Runs once per refresh in the panel task (see display_internal__epd_pull()): rotation is resolved
 * once per row and the panel position stepped per pixel, keeping a full-screen frame fast. */
bruce_result_t display_epd__draw_bitmap(
    int x_start, int y_start, int x_end, int y_end, const bruce_display_color_t *pixels
) {
    xSemaphoreTake(s_drawn_lock, portMAX_DELAY);
    for (int y = y_start; y < y_end; ++y) {
        int16_t **err = s_error_rows;
        if (s_atkinson) {
            bool continues = y == s_error_y && x_start == s_error_x0 && x_end == s_error_x1;
            if (!continues) memset(s_error, 0, sizeof(s_error));
            s_error_y = y + 1;
            s_error_x0 = x_start;
            s_error_x1 = x_end;
        }
        int px, py, dx = 0, dy = 0;
        switch (s_rotation) {
            case 1: px = EPD__WIDTH - 1 - y; py = x_start; dy = 1; break;
            case 2: px = EPD__WIDTH - 1 - x_start; py = EPD__HEIGHT - 1 - y; dx = -1; break;
            case 3: px = y; py = EPD__HEIGHT - 1 - x_start; dy = -1; break;
            default: px = x_start; py = y; dx = 1; break;
        }
        for (int x = x_start; x < x_end; ++x, px += dx, py += dy) {
            unsigned c = *pixels++;
            if ((unsigned)px >= EPD__WIDTH || (unsigned)py >= EPD__HEIGHT) continue;
            unsigned luma = ((c >> 11) * 616u + ((c >> 5) & 0x3Fu) * 600u + (c & 0x1Fu) * 232u) >> 8;
            uint8_t *byte = &s_drawn[py * (EPD__WIDTH / 8) + (px >> 3)];
            uint8_t bit = (uint8_t)(0x80u >> (px & 7));
            bool white;
            if (s_atkinson) {
                /* White is luma 250; 6/8 of the error spreads to the neighbours, the rest is dropped. */
                int i = x - x_start + 2;
                int value = (int)luma + err[0][i];
                white = value >= 125;
                int16_t e = (int16_t)((white ? value - 250 : value) / 8);
                err[0][i + 1] += e;
                err[0][i + 2] += e;
                err[1][i - 1] += e;
                err[1][i] += e;
                err[1][i + 1] += e;
                err[2][i] += e;
            } else {
                white = luma >= (s_noise ? display_epd__noise(px, py) : s_pattern[py & 31][px & 31]);
            }
            if (white != s_invert) *byte |= bit;
            else *byte &= (uint8_t)~bit;
        }
        if (s_atkinson) {
            int16_t *done = err[0];
            err[0] = err[1];
            err[1] = err[2];
            err[2] = done;
            memset(done, 0, sizeof(s_error[0]));
        }
    }
    xSemaphoreGive(s_drawn_lock);
    /* The panel task's own pull lands here too; it must not wake itself for another round. */
    if (xTaskGetCurrentTaskHandle() != s_task) xTaskNotifyGive(s_task);
    /* Nothing is in flight: tell display.c the "transfer" is done right away. */
    (void)display_internal__on_transfer_done_from_isr();
    return BRUCE_OK;
}

bruce_result_t display_epd__invert(bool invert) {
    xSemaphoreTake(s_drawn_lock, portMAX_DELAY);
    if (invert != s_invert) {
        for (int i = 0; i < EPD__BYTES; ++i) s_drawn[i] = (uint8_t)~s_drawn[i];
    }
    s_invert = invert;
    xSemaphoreGive(s_drawn_lock);
    xTaskNotifyGive(s_task);
    return BRUCE_OK;
}

bruce_result_t display_epd__wait_shown(uint32_t timeout_ms) {
    if (s_task == NULL) return BRUCE_OK;
    /* A round already under way may have pulled before the caller's last present; wait for the next one. */
    uint32_t target = s_pull_seq + 1;
    xTaskNotifyGive(s_task);
    TickType_t start = xTaskGetTickCount();
    while ((int32_t)(s_done_seq - target) < 0) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(timeout_ms)) return BRUCE_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return BRUCE_OK;
}

void display_epd__freeze(bool frozen) {
    if (s_task == NULL) return;
    s_frozen = frozen;
    if (frozen) {
        /* Let a refresh already running finish. */
        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreGive(s_panel_lock);
    } else {
        xTaskNotifyGive(s_task);
    }
}

void display_epd__frame_ready(void) {
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

void display_epd__request_full_refresh(void) {
    s_full_requested = true;
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

bruce_result_t display_epd__set_enabled(bool enabled) {
    xSemaphoreTake(s_drawn_lock, portMAX_DELAY);
    s_enabled = enabled;
    xSemaphoreGive(s_drawn_lock);
    if (enabled) xTaskNotifyGive(s_task);
    return BRUCE_OK;
}

#endif
