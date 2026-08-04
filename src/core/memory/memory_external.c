#include "core/memory/memory.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "spi_flash_mmap.h"

#include "core/process/process.h"

#define MEMORY_EXTERNAL__PARTITION_TYPE ((esp_partition_type_t)0x40)
#define MEMORY_EXTERNAL__PARTITION_SUBTYPE ((esp_partition_subtype_t)0x00)
#define MEMORY_EXTERNAL__PARTITION_LABEL "swap"
#define MEMORY_EXTERNAL__FLASH_SECTOR 4096u
#define MEMORY_EXTERNAL__MMU_PAGE 65536u
#define MEMORY_EXTERNAL__MAX_PAGES 32u
#define MEMORY_EXTERNAL__MAX_OBJECTS 32u

typedef struct {
    bruce_memory_backend_t backend;
    size_t size;
    size_t offset;
    size_t page_count;
    void *psram;
    const uint8_t *data;
    const uint8_t *instruction;
    esp_partition_mmap_handle_t mmap_handle;
    bruce_resource_id_t resource_id;
    bruce_process_id_t owner_id;
    uint32_t handle;
    bool executable;
} memory_external__record_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static memory_external__record_t s_records[MEMORY_EXTERNAL__MAX_OBJECTS];
static bool s_pages[MEMORY_EXTERNAL__MAX_PAGES];
static size_t s_next_page;
static uint32_t s_next_handle = 1;

static void memory_external__ensure_mutex(void) {
    if (s_mutex != NULL) return;
    portENTER_CRITICAL(&s_init_mux);
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    portEXIT_CRITICAL(&s_init_mux);
}

static const esp_partition_t *memory_external__partition(void) {
    return esp_partition_find_first(
        MEMORY_EXTERNAL__PARTITION_TYPE, MEMORY_EXTERNAL__PARTITION_SUBTYPE, MEMORY_EXTERNAL__PARTITION_LABEL
    );
}

static memory_external__record_t *memory_external__find_locked(uint32_t handle) {
    if (handle == 0) return NULL;
    for (size_t i = 0; i < MEMORY_EXTERNAL__MAX_OBJECTS; ++i) {
        if (s_records[i].backend != BRUCE_MEMORY_BACKEND_INVALID && s_records[i].handle == handle) {
            return &s_records[i];
        }
    }
    return NULL;
}

static bool
memory_external__matches(const memory_external__record_t *record, const bruce_memory_object_t *object) {
    return record != NULL && object != NULL && record->size == object->size &&
           record->backend == object->backend;
}

static uint32_t memory_external__next_handle_locked(void) {
    for (size_t attempt = 0; attempt <= MEMORY_EXTERNAL__MAX_OBJECTS; ++attempt) {
        uint32_t handle = s_next_handle++;
        if (s_next_handle == 0) s_next_handle = 1;
        if (handle != 0 && memory_external__find_locked(handle) == NULL) return handle;
    }
    return 0;
}

static void memory_external__cleanup(void *context) {
    memory_external__record_t *record = context;
    if (record == NULL || record->backend == BRUCE_MEMORY_BACKEND_INVALID) return;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (record->backend == BRUCE_MEMORY_BACKEND_PSRAM) {
        heap_caps_free(record->psram);
    } else {
        if (record->data != NULL || record->instruction != NULL) {
            esp_partition_munmap(record->mmap_handle);
        }
        size_t first_page = record->offset / MEMORY_EXTERNAL__MMU_PAGE;
        for (size_t i = 0; i < record->page_count; ++i) s_pages[first_page + i] = false;
    }
    memset(record, 0, sizeof(*record));
    xSemaphoreGive(s_mutex);
}

static memory_external__record_t *memory_external__free_record_locked(void) {
    for (size_t i = 0; i < MEMORY_EXTERNAL__MAX_OBJECTS; ++i) {
        if (s_records[i].backend == BRUCE_MEMORY_BACKEND_INVALID) return &s_records[i];
    }
    return NULL;
}

/*
 * esp_partition_write() only invalidates the cache alias matching the
 * physical page's MMU capability (see spi_flash_check_and_flush_cache() /
 * is_page_mapped_in_cache() in IDF's flash_mmap.c): pages mapped executable
 * (ESP_PARTITION_MMAP_INST, used for XIP records here) only get their
 * instruction-bus line invalidated. record->data for an executable record is
 * a separate data-bus alias of the same physical page (spi_flash_phys2cache()
 * with SPI_FLASH_MMAP_DATA), so IDF never invalidates it on write -- any read
 * through it (including the direct_write check below) can keep returning
 * pre-write bytes indefinitely. Invalidate that alias ourselves after writing,
 * and once right after allocating (a fresh mapping can recycle a virtual
 * address a previous, already-torn-down executable record left cached).
 *
 * esp_cache_get_line_size_by_addr() cannot be used to size this: it only
 * recognizes PSRAM (esp_ptr_external_ram) and internal RAM (esp_ptr_internal)
 * addresses and returns 0 -- silently skipping the invalidate -- for a flash
 * mmap alias like this one, which is neither. esp_cache_msync() itself
 * resolves the cache level/line size for any address the cache HAL
 * recognizes (mmu_hal-backed, including flash mmap windows) via
 * cache_hal_vaddr_to_cache_level_id(), so it doesn't need that helper; we
 * only need a conservative alignment for the address range we pass in. 64
 * bytes is a multiple of every supported ESP32-S3 D-cache line size (16/32/64).
 */
#define MEMORY_EXTERNAL__CACHE_ALIGN 64u

static void memory_external__invalidate_data_alias(
    const memory_external__record_t *record, size_t offset, size_t size
) {
    if (!record->executable || record->data == NULL || size == 0) return;
    uintptr_t start = (uintptr_t)(record->data + offset);
    uintptr_t end = (start + size + MEMORY_EXTERNAL__CACHE_ALIGN - 1) &
                    ~(uintptr_t)(MEMORY_EXTERNAL__CACHE_ALIGN - 1);
    start &= ~(uintptr_t)(MEMORY_EXTERNAL__CACHE_ALIGN - 1);
    esp_cache_msync((void *)start, end - start, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

static bool memory_external__allocate_psram_locked(memory_external__record_t *record, size_t size) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size) return false;
    void *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) return false;
    record->backend = BRUCE_MEMORY_BACKEND_PSRAM;
    record->size = size;
    record->psram = data;
    record->data = data;
    return true;
}

static bool
memory_external__allocate_swap_locked(memory_external__record_t *record, size_t size, bool executable) {
    const esp_partition_t *partition = memory_external__partition();
    if (partition == NULL || partition->size % MEMORY_EXTERNAL__MMU_PAGE != 0) return false;
    size_t total_pages = partition->size / MEMORY_EXTERNAL__MMU_PAGE;
    size_t wanted = (size + MEMORY_EXTERNAL__MMU_PAGE - 1u) / MEMORY_EXTERNAL__MMU_PAGE;
    if (total_pages > MEMORY_EXTERNAL__MAX_PAGES || wanted > total_pages) return false;

    size_t first = SIZE_MAX;
    for (size_t pass = 0; pass < total_pages; ++pass) {
        size_t candidate = (s_next_page + pass) % total_pages;
        if (candidate + wanted > total_pages) continue;
        bool available = true;
        for (size_t i = 0; i < wanted; ++i) available = available && !s_pages[candidate + i];
        if (available) {
            first = candidate;
            break;
        }
    }
    if (first == SIZE_MAX) return false;

    for (size_t i = 0; i < wanted; ++i) s_pages[first + i] = true;
    record->backend = BRUCE_MEMORY_BACKEND_SWAP;
    record->size = size;
    record->offset = first * MEMORY_EXTERNAL__MMU_PAGE;
    record->page_count = wanted;
    record->executable = executable;
    s_next_page = (first + wanted) % total_pages;

    if (esp_partition_erase_range(partition, record->offset, wanted * MEMORY_EXTERNAL__MMU_PAGE) != ESP_OK) {
        for (size_t i = 0; i < wanted; ++i) s_pages[first + i] = false;
        memset(record, 0, sizeof(*record));
        return false;
    }

    const void *mapped = NULL;
    esp_partition_mmap_memory_t mapping = executable ? ESP_PARTITION_MMAP_INST : ESP_PARTITION_MMAP_DATA;
    if (esp_partition_mmap(partition, record->offset, record->size, mapping, &mapped, &record->mmap_handle) !=
        ESP_OK) {
        for (size_t i = 0; i < wanted; ++i) s_pages[first + i] = false;
        memset(record, 0, sizeof(*record));
        return false;
    }

    if (executable) {
        record->instruction = mapped;
        record->data = spi_flash_phys2cache(partition->address + record->offset, SPI_FLASH_MMAP_DATA);
        if (record->data == NULL) {
            esp_partition_munmap(record->mmap_handle);
            for (size_t i = 0; i < wanted; ++i) s_pages[first + i] = false;
            memset(record, 0, sizeof(*record));
            return false;
        }
        memory_external__invalidate_data_alias(record, 0, record->size);
    } else {
        record->data = mapped;
    }
    return true;
}

bruce_result_t memory_external__alloc(size_t size, bool executable, bruce_memory_object_t *out_object) {
    if (size == 0 || out_object == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (size > SIZE_MAX - (MEMORY_EXTERNAL__MMU_PAGE - 1u)) return BRUCE_ERR_RESOURCE_LIMIT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memset(out_object, 0, sizeof(*out_object));
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__free_record_locked();
    bool allocated = false;
    if (record != NULL && !executable) { allocated = memory_external__allocate_psram_locked(record, size); }
    if (record != NULL && !allocated) {
        allocated = memory_external__allocate_swap_locked(record, size, executable);
    }
    if (allocated) {
        record->resource_id = BRUCE_RESOURCE_ID_INVALID;
        record->owner_id = owner_id;
        record->handle = memory_external__next_handle_locked();
        allocated = record->handle != 0;
    }
    xSemaphoreGive(s_mutex);
    if (!allocated) {
        if (record != NULL && record->backend != BRUCE_MEMORY_BACKEND_INVALID) {
            memory_external__cleanup(record);
        }
        process_registry__operation_end();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    bruce_resource_id_t resource_id = process_registry__resource_register(memory_external__cleanup, record);
    if (resource_id == BRUCE_RESOURCE_ID_INVALID) {
        memory_external__cleanup(record);
        process_registry__operation_end();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    record->resource_id = resource_id;
    xSemaphoreGive(s_mutex);
    process_registry__account_memory((int64_t)size);
    if (record->backend == BRUCE_MEMORY_BACKEND_SWAP) process_registry__account_swap_memory((int64_t)size);

    *out_object = (bruce_memory_object_t){
        .handle = record->handle,
        .size = record->size,
        .backend = record->backend,
    };
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t memory__external_alloc(size_t size, bruce_memory_object_t *out_object) {
    return memory_external__alloc(size, false, out_object);
}

/* Caller must hold s_mutex and have already validated offset/size against
 * record->size. Shared by the process-owned and Core-owned write entry
 * points below. */
static bruce_result_t memory_external__write_locked(
    memory_external__record_t *record, size_t offset, const void *data, size_t size
) {
    bruce_result_t result = BRUCE_OK;
    if (size != 0 && record->backend == BRUCE_MEMORY_BACKEND_PSRAM) {
        memmove((uint8_t *)record->psram + offset, data, size);
    } else if (size != 0) {
        const esp_partition_t *partition = memory_external__partition();
        const uint8_t *bytes = data;
        uintptr_t source_start = (uintptr_t)data;
        uintptr_t source_end = source_start + size;
        uintptr_t mapping_start = (uintptr_t)record->data;
        uintptr_t mapping_end = mapping_start + record->size;
        if (source_end < source_start || mapping_end < mapping_start ||
            (source_start < mapping_end && source_end > mapping_start)) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        bool direct_write = partition != NULL;
        for (size_t i = 0; direct_write && i < size; ++i) {
            direct_write = (record->data[offset + i] & bytes[i]) == bytes[i];
        }
        if (direct_write) {
            if (esp_partition_write(partition, record->offset + offset, data, size) != ESP_OK) {
                result = BRUCE_ERR_IO;
            }
        } else if (partition != NULL) {
            uint8_t *sector = malloc(MEMORY_EXTERNAL__FLASH_SECTOR);
            if (sector == NULL) {
                result = BRUCE_ERR_NO_MEMORY;
            } else {
                size_t written = 0;
                while (result == BRUCE_OK && written < size) {
                    size_t absolute = record->offset + offset + written;
                    size_t sector_offset = absolute & ~(MEMORY_EXTERNAL__FLASH_SECTOR - 1u);
                    size_t in_sector = absolute - sector_offset;
                    size_t chunk = size - written;
                    if (chunk > MEMORY_EXTERNAL__FLASH_SECTOR - in_sector) {
                        chunk = MEMORY_EXTERNAL__FLASH_SECTOR - in_sector;
                    }
                    if (esp_partition_read(partition, sector_offset, sector, MEMORY_EXTERNAL__FLASH_SECTOR) !=
                        ESP_OK) {
                        result = BRUCE_ERR_IO;
                        break;
                    }
                    memcpy(sector + in_sector, bytes + written, chunk);
                    if (esp_partition_erase_range(partition, sector_offset, MEMORY_EXTERNAL__FLASH_SECTOR) !=
                            ESP_OK ||
                        esp_partition_write(
                            partition, sector_offset, sector, MEMORY_EXTERNAL__FLASH_SECTOR
                        ) != ESP_OK) {
                        result = BRUCE_ERR_IO;
                        break;
                    }
                    written += chunk;
                }
                free(sector);
            }
        } else {
            result = BRUCE_ERR_IO;
        }
        if (result == BRUCE_OK) memory_external__invalidate_data_alias(record, offset, size);
    }
    return result;
}

bruce_result_t
memory__external_write(const bruce_memory_object_t *object, size_t offset, const void *data, size_t size) {
    if (object == NULL || (data == NULL && size != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    if (!memory_external__matches(record, object) || record->owner_id != owner_id || offset > record->size ||
        size > record->size - offset) {
        xSemaphoreGive(s_mutex);
        process_registry__operation_end();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = memory_external__write_locked(record, offset, data, size);
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return result;
}

bruce_result_t memory__external_map(const bruce_memory_object_t *object, const void **out_data) {
    if (object == NULL || out_data == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    if (!memory_external__matches(record, object) || record->owner_id != owner_id) {
        xSemaphoreGive(s_mutex);
        process_registry__operation_end();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_data = record->data;
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t
memory_external__instruction_map(const bruce_memory_object_t *object, const void **out_instruction) {
    if (object == NULL || out_instruction == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    if (!memory_external__matches(record, object) || record->owner_id != owner_id || !record->executable ||
        record->instruction == NULL) {
        xSemaphoreGive(s_mutex);
        process_registry__operation_end();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_instruction = record->instruction;
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t memory_external__adopt(bruce_memory_object_t *object) {
    if (object == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    bool valid = memory_external__matches(record, object);
    bruce_resource_id_t old_resource_id = valid ? record->resource_id : BRUCE_RESOURCE_ID_INVALID;
    bruce_process_id_t old_owner_id = valid ? record->owner_id : BRUCE_PROCESS_ID_INVALID;
    xSemaphoreGive(s_mutex);
    if (!valid) {
        process_registry__operation_end();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_resource_id_t resource_id = BRUCE_RESOURCE_ID_INVALID;
    bruce_result_t ownership_result = process_registry__resource_transfer(
        old_owner_id,
        old_resource_id,
        object->size,
        object->backend == BRUCE_MEMORY_BACKEND_SWAP,
        &resource_id
    );
    if (ownership_result != BRUCE_OK) {
        process_registry__operation_end();
        return ownership_result;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    record = memory_external__find_locked(object->handle);
    valid = memory_external__matches(record, object) && record->resource_id == old_resource_id &&
            record->owner_id == old_owner_id;
    if (valid) {
        record->resource_id = resource_id;
        record->owner_id = owner_id;
    }
    xSemaphoreGive(s_mutex);
    if (!valid) {
        (void)process_registry__resource_release(resource_id);
        process_registry__operation_end();
        return BRUCE_ERR_NOT_FOUND;
    }
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t memory_external__release(bruce_memory_object_t *object) {
    if (object == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    bool valid = memory_external__matches(record, object) && record->owner_id == owner_id;
    bruce_resource_id_t resource_id = valid ? record->resource_id : BRUCE_RESOURCE_ID_INVALID;
    xSemaphoreGive(s_mutex);
    if (!valid) {
        process_registry__operation_end();
        return BRUCE_ERR_PERMISSION;
    }
    if (resource_id != BRUCE_RESOURCE_ID_INVALID) {
        bruce_result_t result = process_registry__resource_release(resource_id);
        if (result != BRUCE_OK) {
            process_registry__operation_end();
            return result;
        }
        process_registry__account_memory(-(int64_t)object->size);
        if (object->backend == BRUCE_MEMORY_BACKEND_SWAP) {
            process_registry__account_swap_memory(-(int64_t)object->size);
        }
    }
    memory_external__cleanup(record);
    memset(object, 0, sizeof(*object));
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t memory__external_free(bruce_memory_object_t *object) {
    return memory_external__release(object);
}

void memory_external__get_swap_stats(size_t *out_total, size_t *out_free, size_t *out_largest) {
    size_t total = 0;
    size_t free_bytes = 0;
    size_t largest = 0;
    if (!process_registry__operation_begin()) {
        if (out_total != NULL) *out_total = 0;
        if (out_free != NULL) *out_free = 0;
        if (out_largest != NULL) *out_largest = 0;
        return;
    }
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const esp_partition_t *partition = memory_external__partition();
    if (partition != NULL && partition->size % MEMORY_EXTERNAL__MMU_PAGE == 0 &&
        partition->size / MEMORY_EXTERNAL__MMU_PAGE <= MEMORY_EXTERNAL__MAX_PAGES) {
        total = partition->size;
        size_t run = 0;
        size_t page_count = partition->size / MEMORY_EXTERNAL__MMU_PAGE;
        for (size_t i = 0; i < page_count; ++i) {
            if (!s_pages[i]) {
                free_bytes += MEMORY_EXTERNAL__MMU_PAGE;
                run += MEMORY_EXTERNAL__MMU_PAGE;
                if (run > largest) largest = run;
            } else {
                run = 0;
            }
        }
    }
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    if (out_total != NULL) *out_total = total;
    if (out_free != NULL) *out_free = free_bytes;
    if (out_largest != NULL) *out_largest = largest;
}

/*
 * Core-owned variants: BRUCE_PROCESS_ID_INVALID marks a record as having no
 * process owner, so these never register a process_registry resource and
 * are never touched by process teardown. process_registry__operation_begin()/
 * end() are still used around every entry point (matching the process-owned
 * path above) purely so a force-kill of whatever task happens to be calling
 * in still waits for that task to leave the guarded section before deleting
 * it - otherwise a kill mid-write could delete the task while it holds
 * s_mutex and wedge every future external-memory call, process-owned or not.
 */

bruce_result_t memory_external_core__alloc(size_t size, bruce_memory_object_t *out_object) {
    if (size == 0 || out_object == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (size > SIZE_MAX - (MEMORY_EXTERNAL__MMU_PAGE - 1u)) return BRUCE_ERR_RESOURCE_LIMIT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    memset(out_object, 0, sizeof(*out_object));
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__free_record_locked();
    bool allocated = false;
    if (record != NULL) allocated = memory_external__allocate_psram_locked(record, size);
    if (record != NULL && !allocated) allocated = memory_external__allocate_swap_locked(record, size, false);
    if (allocated) {
        record->resource_id = BRUCE_RESOURCE_ID_INVALID;
        record->owner_id = BRUCE_PROCESS_ID_INVALID;
        record->handle = memory_external__next_handle_locked();
        allocated = record->handle != 0;
    }
    xSemaphoreGive(s_mutex);
    if (!allocated) {
        if (record != NULL && record->backend != BRUCE_MEMORY_BACKEND_INVALID) {
            memory_external__cleanup(record);
        }
        process_registry__operation_end();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    *out_object = (bruce_memory_object_t){
        .handle = record->handle,
        .size = record->size,
        .backend = record->backend,
    };
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t memory_external_core__write(
    const bruce_memory_object_t *object, size_t offset, const void *data, size_t size
) {
    if (object == NULL || (data == NULL && size != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    if (!memory_external__matches(record, object) || record->owner_id != BRUCE_PROCESS_ID_INVALID ||
        offset > record->size || size > record->size - offset) {
        xSemaphoreGive(s_mutex);
        process_registry__operation_end();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = memory_external__write_locked(record, offset, data, size);
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return result;
}

bruce_result_t memory_external_core__map(const bruce_memory_object_t *object, const void **out_data) {
    if (object == NULL || out_data == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    if (!memory_external__matches(record, object) || record->owner_id != BRUCE_PROCESS_ID_INVALID) {
        xSemaphoreGive(s_mutex);
        process_registry__operation_end();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_data = record->data;
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t memory_external_core__free(bruce_memory_object_t *object) {
    if (object == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    bool valid = memory_external__matches(record, object) && record->owner_id == BRUCE_PROCESS_ID_INVALID;
    xSemaphoreGive(s_mutex);
    if (!valid) {
        process_registry__operation_end();
        return BRUCE_ERR_PERMISSION;
    }
    memory_external__cleanup(record);
    memset(object, 0, sizeof(*object));
    process_registry__operation_end();
    return BRUCE_OK;
}
