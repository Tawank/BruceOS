#include "display_epd.h"

#include "sdkconfig.h"

#if CONFIG_BRUCE_DISPLAY_PANEL_UC8253

#include <string.h>

#include "display_internal.h"

#include "core_sdk/config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
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

#if CONFIG_BRUCE_SD_ENABLED && CONFIG_BRUCE_SD_SPI_HOST == CONFIG_BRUCE_DISPLAY_SPI_HOST
#define EPD__PIN_MISO CONFIG_BRUCE_SD_PIN_MISO
#else
#define EPD__PIN_MISO -1
#endif

/* One bit per pixel, 8 pixels across per byte, 1 = white. */
static uint8_t *s_drawn;   /* What apps have drawn. */
static uint8_t *s_sending; /* Snapshot being sent. */
static uint8_t *s_shown;   /* What the panel shows. */
static SemaphoreHandle_t s_drawn_lock;
static SemaphoreHandle_t s_panel_lock;
static TaskHandle_t s_task;
static spi_device_handle_t s_spi;
static bool s_bus_owned;
static uint8_t s_rotation;
static bool s_invert;
static bool s_enabled = true;
static volatile bool s_full_requested = true; /* The panel's content is unknown at start. */

static void display_epd__send(bool data, const uint8_t *bytes, size_t len) {
    gpio_set_level(EPD__PIN_DC, data);
    while (len > 0) {
        size_t n = len < EPD__CHUNK ? len : EPD__CHUNK;
        spi_transaction_t t = {.length = n * 8, .tx_buffer = bytes};
        /* Interrupt-driven, so the task sleeps through the ~40 ms frame instead of spinning a core. */
        esp_err_t err = spi_device_transmit(s_spi, &t);
        if (err != ESP_OK) ESP_LOGW(TAG, "SPI write of %u bytes failed: %s", (unsigned)n, esp_err_to_name(err));
        bytes += n;
        len -= n;
    }
}

static void display_epd__cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    display_epd__send(false, &cmd, 1);
    if (len > 0) display_epd__send(true, data, len);
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

/* Same order as GxEPD2's GDEQ031T10 driver, which LilyGO's firmware uses:
 * reset, panel setting, image data, then mode, power on and refresh. The
 * panel setting also holds the scan direction: it must only be sent once the
 * controller is out of reset, or that one refresh is drawn flipped. */
static void display_epd__refresh(bool full) {
    gpio_set_level(EPD__PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD__PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    int busy_after_reset = gpio_get_level(EPD__PIN_BUSY);
    display_epd__wait_idle("reset");

    display_epd__cmd(0x00, (const uint8_t[]){0x1F, 0x0D}, 2); /* Panel setting: black/white, LUT from OTP. */
    display_epd__cmd(0x10, s_shown, EPD__BYTES);
    display_epd__cmd(0x13, s_sending, EPD__BYTES);

    if (!full) {
        display_epd__cmd(0xE0, (const uint8_t[]){0x02}, 1); /* Force temperature... */
        display_epd__cmd(0xE5, (const uint8_t[]){0x79}, 1); /* ...to pick the fast partial LUT. */
    }
    display_epd__cmd(0x50, (const uint8_t[]){full ? 0x97 : 0xD7}, 1);
    display_epd__cmd(0x04, NULL, 0); /* Power on. */
    int power_on_ms = display_epd__wait_idle("power on");
    display_epd__cmd(0x12, NULL, 0); /* Refresh. */
    vTaskDelay(pdMS_TO_TICKS(2));
    int busy_in_refresh = gpio_get_level(EPD__PIN_BUSY);
    int refresh_ms = display_epd__wait_idle("refresh");
    display_epd__cmd(0x02, NULL, 0); /* Power off. */
    int power_off_ms = display_epd__wait_idle("power off");
    ESP_LOGD(TAG, "%s refresh: BUSY after reset %d, power on %d ms, BUSY in refresh %d, refresh %d ms, power off %d ms",
             full ? "full" : "partial", busy_after_reset, power_on_ms, busy_in_refresh, refresh_ms, power_off_ms);
}

static void display_epd__task(void *arg) {
    (void)arg;
    int partials = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (int i = 0; i < EPD__SETTLE_MAX && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EPD__SETTLE_MS)); ++i) {}

        display_internal__epd_pull();
        xSemaphoreTake(s_drawn_lock, portMAX_DELAY);
        memcpy(s_sending, s_drawn, EPD__BYTES);
        bool enabled = s_enabled;
        xSemaphoreGive(s_drawn_lock);
        bool full = s_full_requested;
        if (!enabled || (!full && memcmp(s_sending, s_shown, EPD__BYTES) == 0)) continue;

        /* Full refreshes clear the ghosting partial ones leave; see the user setting. */
        int every = config__get_display_full_refresh_every();
        if (every > 0 && partials >= every) full = true;
        s_full_requested = false;
        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        display_epd__refresh(full);
        xSemaphoreGive(s_panel_lock);
        memcpy(s_shown, s_sending, EPD__BYTES);
        partials = full ? 0 : partials + 1;
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
    esp_err_t err = spi_bus_initialize(EPD__HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return BRUCE_ERR_INTERNAL;
    s_bus_owned = err == ESP_OK;

    spi_device_interface_config_t device = {
        .clock_speed_hz = EPD__SPI_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_BRUCE_DISPLAY_PIN_CS,
        .cs_ena_pretrans = 2,
        .cs_ena_posttrans = 2,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 1,
    };
    return spi_bus_add_device(EPD__HOST, &device, &s_spi) == ESP_OK ? BRUCE_OK : BRUCE_ERR_INTERNAL;
}

bruce_result_t display_epd__init(void) {
    s_drawn = heap_caps_malloc(EPD__BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_shown = heap_caps_malloc(EPD__BYTES, MALLOC_CAP_DMA);
    s_sending = heap_caps_malloc(EPD__BYTES, MALLOC_CAP_DMA);
    s_drawn_lock = xSemaphoreCreateMutex();
    s_panel_lock = xSemaphoreCreateMutex();
    if (s_drawn == NULL || s_shown == NULL || s_sending == NULL || s_drawn_lock == NULL || s_panel_lock == NULL) {
        display_epd__deinit();
        return BRUCE_ERR_NO_MEMORY;
    }
    memset(s_drawn, 0xFF, EPD__BYTES);
    memset(s_shown, 0x00, EPD__BYTES); /* Unknown: never equal to the first frame, so it is always drawn. */

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
    s_drawn = s_shown = s_sending = NULL;
}

void display_epd__configure_rotation(uint8_t rotation) { s_rotation = rotation & 3; }

/* 4x4 ordered (Bayer) dither thresholds in panel space, so partial redraws line up with what is
 * already shown. They run 8..248: pure black and white (text, the E-paper theme) never get dots,
 * only grays and colors (games, images) turn into patterns. */
static const uint8_t s_threshold[4][4] = {
    {8, 136, 40, 168}, {200, 72, 232, 104}, {56, 184, 24, 152}, {248, 120, 216, 88}
};

/* Runs in the drawing app's own frame, for every frame: rotation is resolved once per row and the
 * panel position stepped per pixel, keeping a full-screen frame to a few milliseconds. */
bruce_result_t display_epd__draw_bitmap(
    int x_start, int y_start, int x_end, int y_end, const bruce_display_color_t *pixels
) {
    xSemaphoreTake(s_drawn_lock, portMAX_DELAY);
    for (int y = y_start; y < y_end; ++y) {
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
            if ((luma >= s_threshold[py & 3][px & 3]) != s_invert) *byte |= bit;
            else *byte &= (uint8_t)~bit;
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
