#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define STORAGE__PATH_MAX 192

static const char *const TAG = "bruce_storage";
static StaticSemaphore_t s_storage_mutex_storage;
static SemaphoreHandle_t s_storage_mutex;
static portMUX_TYPE s_storage_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_ready;
static bool s_sd_ready;
static bool s_sd_bus_owned;
static int s_sd_host;
static sdmmc_card_t *s_sd_card;

static void storage__lock(void)
{
    if (s_storage_mutex == NULL) {
        portENTER_CRITICAL(&s_storage_init_mux);
        if (s_storage_mutex == NULL) s_storage_mutex = xSemaphoreCreateMutexStatic(&s_storage_mutex_storage);
        portEXIT_CRITICAL(&s_storage_init_mux);
    }
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
}

static void storage__unlock(void)
{
    xSemaphoreGive(s_storage_mutex);
}

bool storage__init(void)
{
    storage__lock();
    if (!s_initialized) {
        const esp_vfs_spiffs_conf_t config = {
            .base_path = STORAGE__MOUNT_PATH,
            .partition_label = "spiffs",
            .max_files = 5,
            .format_if_mount_failed = true,
        };
        esp_err_t err = esp_vfs_spiffs_register(&config);
        s_ready = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
        s_initialized = true;
        if (!s_ready) ESP_LOGE(TAG, "could not mount internal storage: %s", esp_err_to_name(err));
    }
    bool ready = s_ready;
    storage__unlock();
    return ready;
}

static bool storage__is_sd_path(const char *path)
{
    size_t mount_length = strlen(STORAGE__SD_MOUNT_PATH);
    return path != NULL && strncmp(path, STORAGE__SD_MOUNT_PATH, mount_length) == 0 &&
           (path[mount_length] == '\0' || path[mount_length] == '/');
}

static bool storage__is_ready(const char *path)
{
    return path != NULL && (storage__is_sd_path(path) ? s_sd_ready : s_ready);
}

static bool storage__read_file_locked(const char *path, char **data, size_t *size)
{
    if (data == NULL || size == NULL || !storage__is_ready(path)) return false;
    *data = NULL;
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    char *buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        return false;
    }
    size_t read = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(buffer);
        return false;
    }
    buffer[length] = '\0';
    *data = buffer;
    *size = (size_t)length;
    return true;
}

static bool storage__write_file_atomic_locked(const char *path, const void *data, size_t size)
{
    char temporary_path[STORAGE__PATH_MAX];
    if ((data == NULL && size != 0) || !storage__is_ready(path) ||
        snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path) >= (int)sizeof(temporary_path)) return false;
    FILE *file = fopen(temporary_path, "wb");
    if (file == NULL) return false;
    const void *content = data != NULL ? data : "";
    bool written = fwrite(content, 1, size, file) == size && fflush(file) == 0;
    fclose(file);
    if (!written) {
        remove(temporary_path);
        return false;
    }
    remove(path);
    if (rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return false;
    }
    return true;
}

bool storage__exists(const char *path)
{
    storage__lock();
    bool exists = storage__is_ready(path) && access(path, F_OK) == 0;
    storage__unlock();
    return exists;
}

bool storage__read_file(const char *path, char **data, size_t *size)
{
    storage__lock();
    bool read = storage__read_file_locked(path, data, size);
    storage__unlock();
    return read;
}

bool storage__write_file_atomic(const char *path, const void *data, size_t size)
{
    storage__lock();
    bool written = storage__write_file_atomic_locked(path, data, size);
    storage__unlock();
    return written;
}

bool storage__remove(const char *path)
{
    storage__lock();
    bool removed = storage__is_ready(path) && remove(path) == 0;
    storage__unlock();
    return removed;
}

bool storage__rename(const char *from, const char *to)
{
    storage__lock();
    bool renamed = storage__is_ready(from) && storage__is_ready(to) &&
                   storage__is_sd_path(from) == storage__is_sd_path(to) && rename(from, to) == 0;
    storage__unlock();
    return renamed;
}

bool storage__get_usage(const char *path, size_t *total_bytes, size_t *used_bytes)
{
    storage__lock();
    bool known = false;
    if (total_bytes != NULL && used_bytes != NULL && path != NULL) {
        if (storage__is_sd_path(path) && s_sd_ready) {
            uint64_t total = 0;
            uint64_t free_bytes = 0;
            if (esp_vfs_fat_info(STORAGE__SD_MOUNT_PATH, &total, &free_bytes) == ESP_OK) {
                *total_bytes = (size_t)total;
                *used_bytes = (size_t)(total - free_bytes);
                known = true;
            }
        } else if (!storage__is_sd_path(path) && s_ready) {
            known = esp_spiffs_info("spiffs", total_bytes, used_bytes) == ESP_OK;
        }
    }
    storage__unlock();
    return known;
}

bool storage__sd_mount_spi(const storage__sdspi_config_t *config)
{
    if (config == NULL || config->mosi_gpio < 0 || config->miso_gpio < 0 || config->sck_gpio < 0 || config->cs_gpio < 0) return false;
    storage__lock();
    if (s_sd_ready) {
        storage__unlock();
        return true;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = config->miso_gpio,
        .sclk_io_num = config->sck_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(config->host, &bus, SPI_DMA_CH_AUTO);
    s_sd_bus_owned = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "could not initialize SD SPI bus: %s", esp_err_to_name(err));
        storage__unlock();
        return false;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = config->host;
    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.gpio_cs = config->cs_gpio;
    device.host_id = host.slot;
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(STORAGE__SD_MOUNT_PATH, &host, &device, &mount, &s_sd_card);
    if (err != ESP_OK) {
        if (s_sd_bus_owned) spi_bus_free(config->host);
        s_sd_bus_owned = false;
        ESP_LOGW(TAG, "could not mount SD card: %s", esp_err_to_name(err));
        storage__unlock();
        return false;
    }
    s_sd_host = config->host;
    s_sd_ready = true;
    storage__unlock();
    return true;
}

void storage__sd_unmount(void)
{
    storage__lock();
    if (s_sd_ready) {
        esp_vfs_fat_sdcard_unmount(STORAGE__SD_MOUNT_PATH, s_sd_card);
        if (s_sd_bus_owned) spi_bus_free(s_sd_host);
        s_sd_card = NULL;
        s_sd_ready = false;
        s_sd_bus_owned = false;
    }
    storage__unlock();
}

bool storage__sd_is_ready(void)
{
    storage__lock();
    bool ready = s_sd_ready;
    storage__unlock();
    return ready;
}

void storage__free(void *data)
{
    free(data);
}
