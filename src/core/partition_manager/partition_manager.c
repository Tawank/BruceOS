#include "partition_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "spi_flash_mmap.h"

#include "core/process/process.h"

/* ------------------------------------------------------------------------ */
/* Layout constants                                                          */
/* ------------------------------------------------------------------------ */

#define PARTITION_MANAGER__MAX_ENTRIES 8u
#define PARTITION_MANAGER__MAGIC 0x50415254u /* "PART" */
#define PARTITION_MANAGER__VERSION 1u

#define PARTITION_MANAGER__PTABLE_LABEL "ptable"
#define PARTITION_MANAGER__PTABLE_TYPE ((esp_partition_type_t)0x40)
#define PARTITION_MANAGER__PTABLE_SUBTYPE ((esp_partition_subtype_t)0x01)

/* Matches core/memory/memory_external.c's MEMORY_EXTERNAL__PARTITION_*
 * exactly, so a staged "swap" entry is picked up with no changes there. */
#define PARTITION_MANAGER__SWAP_LABEL "swap"
#define PARTITION_MANAGER__SWAP_TYPE ((esp_partition_type_t)0x40)
#define PARTITION_MANAGER__SWAP_SUBTYPE ((esp_partition_subtype_t)0x00)

#define PARTITION_MANAGER__ROOT_LABEL "littlefs"

#define PARTITION_MANAGER__FLAG_FORMAT_PENDING 0x01u

typedef struct __attribute__((packed)) {
    char label[BRUCE_PARTITION_LABEL_MAX];
    uint8_t kind;
    uint8_t flags;
    uint8_t reserved[2];
    uint32_t offset;
    uint32_t size;
} partition_manager__stored_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    partition_manager__stored_entry_t entries[PARTITION_MANAGER__MAX_ENTRIES];
    uint32_t crc32;
} partition_manager__stored_header_t;

static const char *const TAG = "partition_manager";

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_initialized;
static bool s_ready;
static bool s_legacy_materialized;
static bool s_reboot_required;

static uint64_t s_flash_size;
static uint64_t s_user_area_start;   /* Sector-aligned, right after "factory". */
static uint64_t s_data_region_start; /* s_user_area_start + one sector reserved for the header. */

static const esp_partition_t *s_ptable_partition; /* NULL until a table has been committed at least once. */
static const esp_partition_t *s_root_partition;    /* The registered "littlefs" entry, this boot. */

static bruce_partition_entry_t s_entries[PARTITION_MANAGER__MAX_ENTRIES];
static size_t s_entry_count;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static void partition_manager__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void partition_manager__unlock(void) { xSemaphoreGive(s_mutex); }

/* Built-in modules (the "bparted" CLI command) are implicitly trusted with
 * every permission, same rule core/disk/disk.c's disk__mount()/disk__unmount()
 * use; an ELF/JS app can read the table but never mutate flash layout. */
static bool partition_manager__caller_is_built_in(void) {
    bool built_in = false;
    return process_registry__current_context(&built_in, NULL, 0, NULL) == BRUCE_OK && built_in;
}

static bool partition_manager__label_valid(const char *label) {
    size_t length = label != NULL ? strlen(label) : 0;
    if (length == 0 || length >= BRUCE_PARTITION_LABEL_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = label[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-';
        if (!ok) return false;
    }
    return true;
}

static int partition_manager__find_index_locked(const char *label) {
    for (size_t i = 0; i < s_entry_count; ++i) {
        if (strcmp(s_entries[i].label, label) == 0) return (int)i;
    }
    return -1;
}

static uint64_t partition_manager__sector_align_up(uint64_t size) {
    return (size + SPI_FLASH_SEC_SIZE - 1) & ~((uint64_t)SPI_FLASH_SEC_SIZE - 1);
}

/* Same technique core/storage/storage.c's storage__static_partition_end()
 * uses: the end of the last static partition, sector-aligned. */
static uint64_t partition_manager__user_area_start(void) {
    uint64_t end = 0;
    esp_partition_iterator_t iterator =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        uint64_t partition_end = partition->address + partition->size;
        if (partition->flash_chip == esp_flash_default_chip && partition_end > end) end = partition_end;
        iterator = esp_partition_next(iterator);
    }
    return partition_manager__sector_align_up(end);
}

static uint32_t partition_manager__crc32(const partition_manager__stored_header_t *header) {
    partition_manager__stored_header_t copy = *header;
    copy.crc32 = 0;
    return esp_rom_crc32_le(0, (const uint8_t *)&copy, sizeof(copy));
}

static bruce_result_t partition_manager__write_header_locked(const partition_manager__stored_header_t *header) {
    if (s_ptable_partition == NULL) return BRUCE_ERR_INVALID_STATE;
    if (esp_partition_erase_range(s_ptable_partition, 0, SPI_FLASH_SEC_SIZE) != ESP_OK) return BRUCE_ERR_IO;
    if (esp_partition_write(s_ptable_partition, 0, header, sizeof(*header)) != ESP_OK) return BRUCE_ERR_IO;
    partition_manager__stored_header_t verify;
    if (esp_partition_read(s_ptable_partition, 0, &verify, sizeof(verify)) != ESP_OK) return BRUCE_ERR_IO;
    if (memcmp(&verify, header, sizeof(verify)) != 0) return BRUCE_ERR_IO;
    return BRUCE_OK;
}

/* Registers one committed-table entry and, if it carries a pending
 * format left over from the commit that created/reformatted it, erases
 * (swap) or reformats (littlefs) it right now, before anything else can
 * touch it. Only ever runs during boot, before storage__init() mounts
 * anything, so this is the one place a create/delete/format actually takes
 * effect on flash. */
static void partition_manager__register_and_apply_locked(bruce_partition_entry_t *entry, bool *out_applied) {
    uint64_t absolute_offset = s_data_region_start + entry->offset;
    const esp_partition_t *partition = NULL;
    esp_err_t err;
    if (entry->kind == BRUCE_PARTITION_KIND_SWAP) {
        err = esp_partition_register_external(
            NULL, absolute_offset, (size_t)entry->size, PARTITION_MANAGER__SWAP_LABEL,
            PARTITION_MANAGER__SWAP_TYPE, PARTITION_MANAGER__SWAP_SUBTYPE, &partition
        );
    } else {
        err = esp_partition_register_external(
            NULL, absolute_offset, (size_t)entry->size, entry->label, ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &partition
        );
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not register partition '%s': %s", entry->label, esp_err_to_name(err));
        return;
    }
    if (entry->kind == BRUCE_PARTITION_KIND_LITTLEFS && strcmp(entry->label, PARTITION_MANAGER__ROOT_LABEL) == 0) {
        s_root_partition = partition;
    }
    if (entry->format_pending) {
        ESP_LOGI(TAG, "applying pending format for '%s'", entry->label);
        if (entry->kind == BRUCE_PARTITION_KIND_SWAP) {
            (void)esp_partition_erase_range(partition, 0, partition->size);
        } else {
            (void)esp_littlefs_format_partition(partition);
        }
        entry->format_pending = false;
        *out_applied = true;
    }
}

static bruce_result_t partition_manager__ensure_init_locked(void) {
    if (s_initialized) return s_ready ? BRUCE_OK : BRUCE_ERR_IO;
    s_initialized = true;

    uint32_t flash_size32 = 0;
    if (esp_flash_get_size(NULL, &flash_size32) != ESP_OK) return BRUCE_ERR_IO;
    s_flash_size = flash_size32;
    s_user_area_start = partition_manager__user_area_start();
    if (s_user_area_start >= s_flash_size) return BRUCE_ERR_INVALID_STATE;
    s_data_region_start = s_user_area_start + SPI_FLASH_SEC_SIZE;

    const esp_partition_t *ptable = NULL;
    esp_err_t ptable_err = esp_partition_register_external(
        NULL, s_user_area_start, SPI_FLASH_SEC_SIZE, PARTITION_MANAGER__PTABLE_LABEL,
        PARTITION_MANAGER__PTABLE_TYPE, PARTITION_MANAGER__PTABLE_SUBTYPE, &ptable
    );

    partition_manager__stored_header_t header;
    bool have_table = false;
    if (ptable_err == ESP_OK && esp_partition_read(ptable, 0, &header, sizeof(header)) == ESP_OK &&
        header.magic == PARTITION_MANAGER__MAGIC && header.version == PARTITION_MANAGER__VERSION &&
        header.entry_count <= PARTITION_MANAGER__MAX_ENTRIES &&
        partition_manager__crc32(&header) == header.crc32) {
        have_table = true;
    }

    if (have_table) {
        s_ptable_partition = ptable;
        s_entry_count = 0;
        for (uint32_t i = 0; i < header.entry_count; ++i) {
            partition_manager__stored_entry_t *stored = &header.entries[i];
            bruce_partition_entry_t *entry = &s_entries[s_entry_count++];
            memset(entry, 0, sizeof(*entry));
            strncpy(entry->label, stored->label, BRUCE_PARTITION_LABEL_MAX - 1);
            entry->kind = (bruce_partition_kind_t)stored->kind;
            entry->offset = stored->offset;
            entry->size = stored->size;
            entry->format_pending = (stored->flags & PARTITION_MANAGER__FLAG_FORMAT_PENDING) != 0;
        }
        bool applied_pending = false;
        for (size_t i = 0; i < s_entry_count; ++i) {
            partition_manager__register_and_apply_locked(&s_entries[i], &applied_pending);
        }
        if (applied_pending) {
            for (uint32_t i = 0; i < header.entry_count; ++i) {
                header.entries[i].flags &= (uint8_t) ~PARTITION_MANAGER__FLAG_FORMAT_PENDING;
            }
            header.crc32 = partition_manager__crc32(&header);
            /* Best-effort: if this write fails, the format is already
             * applied and merely re-runs (harmlessly) next boot too. */
            (void)partition_manager__write_header_locked(&header);
        }
        s_ready = true;
    } else {
        if (ptable_err == ESP_OK) {
            /* No valid table yet: undo the header reservation so a device
             * that has never used the partition manager keeps the exact
             * legacy layout (the whole user area as one littlefs partition,
             * starting right at s_user_area_start with no reserved sector). */
            (void)esp_partition_deregister_external(ptable);
        }
        s_ptable_partition = NULL;
        s_entry_count = 1;
        bruce_partition_entry_t *entry = &s_entries[0];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->label, sizeof(entry->label), "%s", PARTITION_MANAGER__ROOT_LABEL);
        entry->kind = BRUCE_PARTITION_KIND_LITTLEFS;
        entry->offset = 0;
        entry->size = s_flash_size - s_user_area_start;
        entry->format_pending = false;

        const esp_partition_t *root = NULL;
        esp_err_t err = esp_partition_register_external(
            NULL, s_user_area_start, (size_t)entry->size, entry->label, ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &root
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not register legacy littlefs region: %s", esp_err_to_name(err));
            return BRUCE_ERR_IO;
        }
        s_root_partition = root;
        s_ready = true;
    }
    return BRUCE_OK;
}

/* The first stage_*() call on a device that has never committed a table
 * converts the single in-memory legacy entry to the same addressing
 * convention a committed table uses (offsets relative to
 * s_data_region_start, i.e. after the reserved header sector). This is the
 * one unavoidable side effect of formatting for the first time: the root
 * filesystem's usable size shrinks by one flash sector (4096 bytes) to make
 * room for the table itself. Idempotent; only touches state in RAM. */
static void partition_manager__materialize_locked(void) {
    if (s_ptable_partition != NULL || s_legacy_materialized) return;
    if (s_entry_count == 1 && strcmp(s_entries[0].label, PARTITION_MANAGER__ROOT_LABEL) == 0 &&
        s_entries[0].kind == BRUCE_PARTITION_KIND_LITTLEFS) {
        s_entries[0].offset = 0;
        s_entries[0].size = s_flash_size - s_data_region_start;
    }
    s_legacy_materialized = true;
}

/* ------------------------------------------------------------------------ */
/* Core-private entry point (storage__init())                                */
/* ------------------------------------------------------------------------ */

bruce_result_t partition_manager__init_for_storage(const esp_partition_t **out_root_littlefs) {
    if (out_root_littlefs == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_root_littlefs = NULL;
    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) *out_root_littlefs = s_root_partition;
    partition_manager__unlock();
    return result;
}

/* ------------------------------------------------------------------------ */
/* Public SDK surface                                                        */
/* ------------------------------------------------------------------------ */

bruce_result_t
partition_manager__list(bruce_partition_entry_t *entries, size_t capacity, size_t *out_count) {
    if (out_count == NULL || (entries == NULL && capacity != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) {
        *out_count = s_entry_count;
        size_t copy_count = s_entry_count < capacity ? s_entry_count : capacity;
        if (entries != NULL && copy_count > 0) memcpy(entries, s_entries, copy_count * sizeof(*entries));
        if (entries != NULL && capacity < s_entry_count) result = BRUCE_ERR_RESOURCE_LIMIT;
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__free_space(uint64_t *out_bytes) {
    if (out_bytes == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) {
        /* Always measured against the committed-table convention (data
         * region starts after the reserved header sector) - that's the
         * layout that applies once anything is staged, even on a device
         * still running the legacy single-entry layout this boot. The
         * not-yet-materialized legacy root entry is addressed relative to
         * s_user_area_start (no reserved header), so it's excluded here
         * rather than folded into this convention's high-water mark. */
        uint64_t used_end = 0;
        for (size_t i = 0; i < s_entry_count; ++i) {
            bool is_unmaterialized_legacy_root =
                s_ptable_partition == NULL && !s_legacy_materialized && i == 0;
            uint64_t end = is_unmaterialized_legacy_root ? 0 : s_entries[i].offset + s_entries[i].size;
            if (end > used_end) used_end = end;
        }
        uint64_t data_region_size = s_flash_size - s_data_region_start;
        *out_bytes = data_region_size > used_end ? data_region_size - used_end : 0;
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t
partition_manager__stage_create(const char *label, bruce_partition_kind_t kind, uint64_t size_bytes) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (!partition_manager__label_valid(label) ||
        (kind != BRUCE_PARTITION_KIND_SWAP && kind != BRUCE_PARTITION_KIND_LITTLEFS) || size_bytes == 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bool is_swap_label = strcmp(label, PARTITION_MANAGER__SWAP_LABEL) == 0;
    if ((kind == BRUCE_PARTITION_KIND_SWAP) != is_swap_label) return BRUCE_ERR_INVALID_ARGUMENT;

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) partition_manager__materialize_locked();
    if (result == BRUCE_OK && s_entry_count >= PARTITION_MANAGER__MAX_ENTRIES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK && partition_manager__find_index_locked(label) >= 0) result = BRUCE_ERR_ALREADY_EXISTS;
    if (result == BRUCE_OK) {
        uint64_t aligned_size = partition_manager__sector_align_up(size_bytes);
        uint64_t high_water = 0;
        for (size_t i = 0; i < s_entry_count; ++i) {
            uint64_t end = s_entries[i].offset + s_entries[i].size;
            if (end > high_water) high_water = end;
        }
        uint64_t data_region_size = s_flash_size - s_data_region_start;
        if (high_water + aligned_size > data_region_size) {
            result = BRUCE_ERR_RESOURCE_LIMIT;
        } else {
            bruce_partition_entry_t *entry = &s_entries[s_entry_count++];
            memset(entry, 0, sizeof(*entry));
            snprintf(entry->label, sizeof(entry->label), "%s", label);
            entry->kind = kind;
            entry->offset = high_water;
            entry->size = aligned_size;
            entry->format_pending = true;
            s_reboot_required = true;
        }
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__stage_delete(const char *label) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (label == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) partition_manager__materialize_locked();
    int index = result == BRUCE_OK ? partition_manager__find_index_locked(label) : -1;
    if (result == BRUCE_OK && index < 0) result = BRUCE_ERR_NOT_FOUND;
    if (result == BRUCE_OK && strcmp(label, PARTITION_MANAGER__ROOT_LABEL) == 0) result = BRUCE_ERR_PERMISSION;
    if (result == BRUCE_OK) {
        size_t last = s_entry_count - 1;
        if ((size_t)index != last) s_entries[index] = s_entries[last];
        s_entry_count--;
        s_reboot_required = true;
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__stage_format(const char *label) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (label == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) partition_manager__materialize_locked();
    int index = result == BRUCE_OK ? partition_manager__find_index_locked(label) : -1;
    if (result == BRUCE_OK && index < 0) result = BRUCE_ERR_NOT_FOUND;
    if (result == BRUCE_OK) {
        s_entries[index].format_pending = true;
        s_reboot_required = true;
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__commit(void) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) partition_manager__materialize_locked();
    if (result == BRUCE_OK && s_entry_count == 0) result = BRUCE_ERR_INVALID_STATE;

    if (result == BRUCE_OK && s_ptable_partition == NULL) {
        const esp_partition_t *ptable = NULL;
        esp_err_t err = esp_partition_register_external(
            NULL, s_user_area_start, SPI_FLASH_SEC_SIZE, PARTITION_MANAGER__PTABLE_LABEL,
            PARTITION_MANAGER__PTABLE_TYPE, PARTITION_MANAGER__PTABLE_SUBTYPE, &ptable
        );
        if (err != ESP_OK) result = BRUCE_ERR_IO;
        else s_ptable_partition = ptable;
    }

    if (result == BRUCE_OK) {
        partition_manager__stored_header_t header;
        memset(&header, 0, sizeof(header));
        header.magic = PARTITION_MANAGER__MAGIC;
        header.version = PARTITION_MANAGER__VERSION;
        header.entry_count = (uint32_t)s_entry_count;
        for (size_t i = 0; i < s_entry_count; ++i) {
            partition_manager__stored_entry_t *stored = &header.entries[i];
            memset(stored, 0, sizeof(*stored));
            strncpy(stored->label, s_entries[i].label, BRUCE_PARTITION_LABEL_MAX - 1);
            stored->kind = (uint8_t)s_entries[i].kind;
            stored->flags = s_entries[i].format_pending ? PARTITION_MANAGER__FLAG_FORMAT_PENDING : 0;
            stored->offset = (uint32_t)s_entries[i].offset;
            stored->size = (uint32_t)s_entries[i].size;
        }
        header.crc32 = partition_manager__crc32(&header);
        result = partition_manager__write_header_locked(&header);
    }
    partition_manager__unlock();
    return result;
}

bool partition_manager__reboot_required(void) {
    partition_manager__lock();
    bool required = s_reboot_required;
    partition_manager__unlock();
    return required;
}
