#include "core_sdk/loader.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "core/process/process.h"
#include "core_sdk/storage.h"

#define LOADER_IMAGE__PARTITION_TYPE ((esp_partition_type_t)0x40)
#define LOADER_IMAGE__PARTITION_SUBTYPE ((esp_partition_subtype_t)0x00)
#define LOADER_IMAGE__PARTITION_LABEL "elf_stage"
#define LOADER_IMAGE__IO_CHUNK 4096u

typedef struct {
    bool active;
    bool mapped;
    const uint8_t *data;
    size_t size;
    esp_partition_mmap_handle_t mmap_handle;
    bruce_resource_id_t resource_id;
    uint32_t public_handle;
} loader_image_slot_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static loader_image_slot_t s_slot;
static uint32_t s_next_handle = 1;

static void loader_image__ensure_mutex(void) {
    if (s_mutex != NULL) return;
    portENTER_CRITICAL(&s_init_mux);
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    portEXIT_CRITICAL(&s_init_mux);
}

static void loader_image__cleanup(void *context) {
    loader_image_slot_t *slot = context;
    if (slot == NULL || !slot->active) return;
    if (slot->mapped) esp_partition_munmap(slot->mmap_handle);
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(s_mutex);
}

static bruce_result_t loader_image__fail(bruce_result_t result) {
    bruce_resource_id_t resource_id = s_slot.resource_id;
    if (resource_id != BRUCE_RESOURCE_ID_INVALID) (void)process_registry__resource_release(resource_id);
    loader_image__cleanup(&s_slot);
    return result;
}

bruce_result_t loader__stage_path(const char *path, bruce_loader_image_t *out_image) {
    if (path == NULL || out_image == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(out_image, 0, sizeof(*out_image));
    loader_image__ensure_mutex();
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return BRUCE_ERR_BUSY;

    memset(&s_slot, 0, sizeof(s_slot));
    s_slot.active = true;
    s_slot.resource_id = process_registry__resource_register(loader_image__cleanup, &s_slot);
    if (s_slot.resource_id == BRUCE_RESOURCE_ID_INVALID) {
        loader_image__cleanup(&s_slot);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    const esp_partition_t *partition = esp_partition_find_first(
        LOADER_IMAGE__PARTITION_TYPE, LOADER_IMAGE__PARTITION_SUBTYPE, LOADER_IMAGE__PARTITION_LABEL
    );
    if (partition == NULL) return loader_image__fail(BRUCE_ERR_NOT_FOUND);

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    uint64_t file_size = 0;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_END, &file_size);
    if (result == BRUCE_OK && (file_size == 0 || file_size > partition->size || file_size > SIZE_MAX)) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
    }
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);

    size_t size = (size_t)file_size;
    size_t erase_size = partition->erase_size;
    size_t erase_length = 0;
    if (result == BRUCE_OK) {
        if (size > SIZE_MAX - (erase_size - 1u)) result = BRUCE_ERR_RESOURCE_LIMIT;
        else erase_length = ((size + erase_size - 1u) / erase_size) * erase_size;
    }
    if (result == BRUCE_OK && esp_partition_erase_range(partition, 0, erase_length) != ESP_OK) {
        result = BRUCE_ERR_IO;
    }

    uint8_t *buffer = result == BRUCE_OK ? malloc(LOADER_IMAGE__IO_CHUNK) : NULL;
    if (result == BRUCE_OK && buffer == NULL) result = BRUCE_ERR_NO_MEMORY;
    uint32_t source_crc = 0;
    size_t offset = 0;
    while (result == BRUCE_OK && offset < size) {
        size_t wanted = size - offset < LOADER_IMAGE__IO_CHUNK ? size - offset : LOADER_IMAGE__IO_CHUNK;
        size_t received = 0;
        result = storage__read(file, buffer, wanted, &received);
        if (result != BRUCE_OK || received != wanted) {
            if (result == BRUCE_OK) result = BRUCE_ERR_IO;
            break;
        }
        source_crc = esp_rom_crc32_le(source_crc, buffer, received);
        if (esp_partition_write(partition, offset, buffer, received) != ESP_OK) {
            result = BRUCE_ERR_IO;
            break;
        }
        offset += received;
    }
    free(buffer);
    if (file != BRUCE_FILE_ID_INVALID) {
        bruce_result_t close_result = storage__close(file);
        if (result == BRUCE_OK) result = close_result;
    }
    if (result != BRUCE_OK) return loader_image__fail(result);

    const void *mapped = NULL;
    if (esp_partition_mmap(
            partition, 0, size, ESP_PARTITION_MMAP_DATA, &mapped, &s_slot.mmap_handle
        ) != ESP_OK) {
        return loader_image__fail(BRUCE_ERR_NO_MEMORY);
    }
    s_slot.mapped = true;
    s_slot.data = mapped;
    s_slot.size = size;
    uint32_t mapped_crc = esp_rom_crc32_le(0, mapped, size);
    if (mapped_crc != source_crc) return loader_image__fail(BRUCE_ERR_IO);

    s_slot.public_handle = s_next_handle++;
    if (s_next_handle == 0) s_next_handle = 1;
    out_image->data = s_slot.data;
    out_image->size = s_slot.size;
    out_image->handle = s_slot.public_handle;
    return BRUCE_OK;
}

bruce_result_t loader__release_image(bruce_loader_image_t *image) {
    if (image == NULL || image->handle == 0 || !s_slot.active || image->handle != s_slot.public_handle) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_resource_id_t resource_id = s_slot.resource_id;
    bruce_result_t result = process_registry__resource_release(resource_id);
    if (result != BRUCE_OK) return result;
    loader_image__cleanup(&s_slot);
    memset(image, 0, sizeof(*image));
    return BRUCE_OK;
}
