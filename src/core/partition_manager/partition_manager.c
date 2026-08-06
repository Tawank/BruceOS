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
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

/*
 *  [ static partitions ][ user area .............................. ][ptable]
 *                       ^ s_user_area_start                          ^ last
 *                                                                      sector
 *
 * Entry offsets are relative to s_user_area_start; the table itself lives in
 * the last sector of the flash so that reserving it never moves or resizes
 * the root partition - a device that has never committed a table sees the
 * exact same "littlefs" geometry as one that has, and the first commit
 * therefore costs no reformat.
 */

#define PARTITION_MANAGER__MAX_ENTRIES BRUCE_PARTITION_MAX_ENTRIES
#define PARTITION_MANAGER__MAGIC 0x50415254u /* "PART" */
#define PARTITION_MANAGER__VERSION 2u

#define PARTITION_MANAGER__PTABLE_LABEL "ptable"
#define PARTITION_MANAGER__PTABLE_TYPE ((esp_partition_type_t)0x40)
#define PARTITION_MANAGER__PTABLE_SUBTYPE ((esp_partition_subtype_t)0x01)

/* Matches core/memory/memory_external.c's MEMORY_EXTERNAL__PARTITION_*
 * exactly, so a committed "swap" entry is picked up with no changes there. */
#define PARTITION_MANAGER__SWAP_TYPE ((esp_partition_type_t)0x40)
#define PARTITION_MANAGER__SWAP_SUBTYPE ((esp_partition_subtype_t)0x00)

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

/* The in-RAM shape of one entry. `format_explicit` is what stage_format()
 * (and creating a partition, which always starts formatted) sets; the root
 * entry additionally counts as pending-format whenever its size differs
 * from the size it has this boot, because shrinking or growing a LittleFS
 * volume in place is not possible - see partition_manager__format_pending(). */
typedef struct {
    char label[BRUCE_PARTITION_LABEL_MAX];
    bruce_partition_kind_t kind;
    uint64_t offset;
    uint64_t size;
    bool format_explicit;
} partition_manager__entry_t;

typedef struct {
    partition_manager__entry_t entries[PARTITION_MANAGER__MAX_ENTRIES];
    size_t count;
} partition_manager__table_t;

static const char *const TAG = "partition_manager";

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_initialized;
static bool s_ready;

static uint64_t s_flash_size;
static uint64_t s_user_area_start; /* Sector-aligned, right after the last static partition. */
static uint64_t s_ptable_offset;   /* Last sector of the flash. */
static uint64_t s_data_region_size;

static const esp_partition_t *s_ptable_partition;
static const esp_partition_t *s_root_partition; /* The registered root entry, this boot. */

/* The layout this boot actually runs (list_current()), the one on flash
 * (which the next boot will run), and the one being edited (list_planned()).
 * stage_*() only ever touches s_working; commit() copies it over s_committed
 * and writes flash; nothing but a reboot can change s_boot. */
static partition_manager__table_t s_boot;
static partition_manager__table_t s_committed;
static partition_manager__table_t s_working;

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

/* Built-in modules (the "bparted" command) are implicitly trusted with every
 * permission, the same rule core/disk/disk.c's disk__mount()/disk__unmount()
 * use; an ELF/JS app can read the layout but never mutate it. */
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

static bool partition_manager__is_root(const char *label) {
    return strcmp(label, BRUCE_PARTITION_ROOT_LABEL) == 0;
}

static uint64_t partition_manager__sector_align_up(uint64_t size) {
    return (size + SPI_FLASH_SEC_SIZE - 1) & ~((uint64_t)SPI_FLASH_SEC_SIZE - 1);
}

static int partition_manager__find(const partition_manager__table_t *table, const char *label) {
    for (size_t i = 0; i < table->count; ++i) {
        if (strcmp(table->entries[i].label, label) == 0) return (int)i;
    }
    return -1;
}

/* The root partition's size this boot, i.e. what a planned root entry has to
 * match to avoid being reformatted. Zero when there is no boot table yet
 * (during ensure_init_locked() itself), which makes every comparison against
 * it false - exactly right, since nothing is staged at that point. */
static uint64_t partition_manager__boot_root_size(void) {
    int index = partition_manager__find(&s_boot, BRUCE_PARTITION_ROOT_LABEL);
    return index < 0 ? 0 : s_boot.entries[index].size;
}

/* Whether `entry` is erased/reformatted on the next boot. Explicit for a
 * staged format or a partition that does not exist yet, derived for the root
 * entry whenever a create/delete resized it. */
static bool partition_manager__format_pending(const partition_manager__entry_t *entry) {
    if (entry->format_explicit) return true;
    return partition_manager__is_root(entry->label) && entry->size != partition_manager__boot_root_size();
}

/* Ignores entry order: the same layout reached by different create/delete
 * sequences must compare equal, or an edit that ends up back where it
 * started would still claim a reboot is required. */
static bool partition_manager__tables_equal(
    const partition_manager__table_t *left, const partition_manager__table_t *right
) {
    if (left->count != right->count) return false;
    for (size_t i = 0; i < left->count; ++i) {
        const partition_manager__entry_t *a = &left->entries[i];
        int index = partition_manager__find(right, a->label);
        if (index < 0) return false;
        const partition_manager__entry_t *b = &right->entries[index];
        if (a->kind != b->kind || a->offset != b->offset || a->size != b->size) return false;
        if (partition_manager__format_pending(a) != partition_manager__format_pending(b)) return false;
    }
    return true;
}

/* The root entry always starts at 0 and spans everything below the lowest
 * extra partition, so it absorbs the space a delete frees directly above it
 * and gives space back to a create. Called after every table edit. */
static void partition_manager__recompute_root(partition_manager__table_t *table) {
    int root = partition_manager__find(table, BRUCE_PARTITION_ROOT_LABEL);
    if (root < 0) return;
    uint64_t limit = s_data_region_size;
    for (size_t i = 0; i < table->count; ++i) {
        if ((int)i == root) continue;
        if (table->entries[i].offset < limit) limit = table->entries[i].offset;
    }
    table->entries[root].offset = 0;
    table->entries[root].size = limit;
}

/* Walks the free spans of `table` from the top of the user area downwards,
 * calling back with each one. The lowest span stops at
 * BRUCE_PARTITION_ROOT_MIN_BYTES: everything below the lowest extra
 * partition belongs to the root entry, which always keeps that much. */
static void partition_manager__each_free_span(
    const partition_manager__table_t *table, void (*visit)(uint64_t start, uint64_t end, void *context),
    void *context
) {
    /* Extra partitions, sorted by offset ascending (at most 7 of them). */
    uint64_t starts[PARTITION_MANAGER__MAX_ENTRIES];
    uint64_t ends[PARTITION_MANAGER__MAX_ENTRIES];
    size_t count = 0;
    for (size_t i = 0; i < table->count; ++i) {
        if (partition_manager__is_root(table->entries[i].label)) continue;
        uint64_t offset = table->entries[i].offset;
        size_t at = count;
        while (at > 0 && starts[at - 1] > offset) {
            starts[at] = starts[at - 1];
            ends[at] = ends[at - 1];
            at--;
        }
        starts[at] = offset;
        ends[at] = offset + table->entries[i].size;
        count++;
    }

    uint64_t span_end = s_data_region_size;
    for (size_t i = count; i > 0; --i) {
        if (span_end > ends[i - 1]) visit(ends[i - 1], span_end, context);
        span_end = starts[i - 1];
    }
    if (span_end > BRUCE_PARTITION_ROOT_MIN_BYTES) visit(BRUCE_PARTITION_ROOT_MIN_BYTES, span_end, context);
}

typedef struct {
    uint64_t wanted;  /* Non-zero when looking for a span to place a partition in. */
    uint64_t largest; /* Largest span seen. */
    uint64_t offset;  /* Where the wanted size fits, once found. */
    bool found;
} partition_manager__span_search_t;

static void partition_manager__span_visitor(uint64_t start, uint64_t end, void *context) {
    partition_manager__span_search_t *search = context;
    uint64_t length = end - start;
    if (length > search->largest) search->largest = length;
    /* Spans are visited top-down and a new partition is placed at the top of
     * the first one that fits, so the root entry keeps one contiguous run. */
    if (!search->found && search->wanted != 0 && length >= search->wanted) {
        search->offset = end - search->wanted;
        search->found = true;
    }
}

/* Largest partition stage_create() would accept against `table`. */
static uint64_t partition_manager__max_new_size(const partition_manager__table_t *table) {
    if (table->count >= PARTITION_MANAGER__MAX_ENTRIES) return 0;
    partition_manager__span_search_t search = {0};
    partition_manager__each_free_span(table, partition_manager__span_visitor, &search);
    return search.largest;
}

/* ------------------------------------------------------------------------ */
/* Flash access                                                              */
/* ------------------------------------------------------------------------ */

static uint32_t partition_manager__crc32(const partition_manager__stored_header_t *header) {
    partition_manager__stored_header_t copy = *header;
    copy.crc32 = 0;
    return esp_rom_crc32_le(0, (const uint8_t *)&copy, sizeof(copy));
}

static bruce_result_t partition_manager__write_header(const partition_manager__stored_header_t *header) {
    if (s_ptable_partition == NULL) return BRUCE_ERR_INVALID_STATE;
    if (esp_partition_erase_range(s_ptable_partition, 0, SPI_FLASH_SEC_SIZE) != ESP_OK) return BRUCE_ERR_IO;
    if (esp_partition_write(s_ptable_partition, 0, header, sizeof(*header)) != ESP_OK) return BRUCE_ERR_IO;
    partition_manager__stored_header_t verify;
    if (esp_partition_read(s_ptable_partition, 0, &verify, sizeof(verify)) != ESP_OK) return BRUCE_ERR_IO;
    if (memcmp(&verify, header, sizeof(verify)) != 0) return BRUCE_ERR_IO;
    return BRUCE_OK;
}

static void
partition_manager__fill_header(const partition_manager__table_t *table, partition_manager__stored_header_t *header) {
    memset(header, 0, sizeof(*header));
    header->magic = PARTITION_MANAGER__MAGIC;
    header->version = PARTITION_MANAGER__VERSION;
    header->entry_count = (uint32_t)table->count;
    for (size_t i = 0; i < table->count; ++i) {
        partition_manager__stored_entry_t *stored = &header->entries[i];
        strncpy(stored->label, table->entries[i].label, BRUCE_PARTITION_LABEL_MAX - 1);
        stored->kind = (uint8_t)table->entries[i].kind;
        stored->flags =
            partition_manager__format_pending(&table->entries[i]) ? PARTITION_MANAGER__FLAG_FORMAT_PENDING : 0;
        stored->offset = (uint32_t)table->entries[i].offset;
        stored->size = (uint32_t)table->entries[i].size;
    }
    header->crc32 = partition_manager__crc32(header);
}

/* A table that is structurally impossible to run - a missing or misplaced
 * root entry, an out-of-range or overlapping partition - is refused rather
 * than registered, both when read back from flash (where it means corruption
 * a CRC cannot catch, e.g. a table written by a newer layout) and before
 * commit() writes one. */
static bool partition_manager__table_valid(const partition_manager__table_t *table) {
    if (table->count == 0 || table->count > PARTITION_MANAGER__MAX_ENTRIES) return false;
    int root = partition_manager__find(table, BRUCE_PARTITION_ROOT_LABEL);
    if (root < 0 || table->entries[root].kind != BRUCE_PARTITION_KIND_LITTLEFS ||
        table->entries[root].offset != 0 || table->entries[root].size < BRUCE_PARTITION_ROOT_MIN_BYTES) {
        return false;
    }
    for (size_t i = 0; i < table->count; ++i) {
        const partition_manager__entry_t *entry = &table->entries[i];
        if (!partition_manager__label_valid(entry->label)) return false;
        if (entry->size == 0 || entry->offset + entry->size > s_data_region_size) return false;
        if ((entry->offset % SPI_FLASH_SEC_SIZE) != 0 || (entry->size % SPI_FLASH_SEC_SIZE) != 0) return false;
        bool is_swap_kind = entry->kind == BRUCE_PARTITION_KIND_SWAP;
        if (is_swap_kind != (strcmp(entry->label, BRUCE_PARTITION_SWAP_LABEL) == 0)) return false;
        for (size_t j = i + 1; j < table->count; ++j) {
            const partition_manager__entry_t *other = &table->entries[j];
            if (strcmp(entry->label, other->label) == 0) return false;
            if (entry->offset < other->offset + other->size && other->offset < entry->offset + entry->size) {
                return false;
            }
        }
    }
    return true;
}

/* Registers one boot-table entry and, when it carries a pending format,
 * erases (swap) or reformats (littlefs) it right now. Only ever runs during
 * ensure_init_locked(), before storage__init() mounts anything, so this is
 * the one place a create/delete/format actually takes effect on flash. */
static void
partition_manager__register_and_apply(partition_manager__entry_t *entry, bool format_pending, bool *out_applied) {
    uint64_t absolute_offset = s_user_area_start + entry->offset;
    const esp_partition_t *partition = NULL;
    esp_err_t err;
    if (entry->kind == BRUCE_PARTITION_KIND_SWAP) {
        err = esp_partition_register_external(
            NULL, absolute_offset, (size_t)entry->size, BRUCE_PARTITION_SWAP_LABEL,
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
    if (partition_manager__is_root(entry->label)) s_root_partition = partition;
    if (!format_pending) return;

    ESP_LOGI(TAG, "applying pending format for '%s'", entry->label);
    if (entry->kind == BRUCE_PARTITION_KIND_SWAP) {
        (void)esp_partition_erase_range(partition, 0, partition->size);
    } else {
        (void)esp_littlefs_format_partition(partition);
    }
    *out_applied = true;
}

/* The layout every device starts from: the whole user area as one root
 * LittleFS volume. Identical to what a device that has never committed a
 * table has always run, and to what commit() writes as entry 0. */
static void partition_manager__default_table(partition_manager__table_t *table) {
    memset(table, 0, sizeof(*table));
    table->count = 1;
    snprintf(table->entries[0].label, sizeof(table->entries[0].label), "%s", BRUCE_PARTITION_ROOT_LABEL);
    table->entries[0].kind = BRUCE_PARTITION_KIND_LITTLEFS;
    table->entries[0].offset = 0;
    table->entries[0].size = s_data_region_size;
}

static bruce_result_t partition_manager__ensure_init_locked(void) {
    if (s_initialized) return s_ready ? BRUCE_OK : BRUCE_ERR_IO;
    s_initialized = true;

    uint32_t flash_size32 = 0;
    if (esp_flash_get_size(NULL, &flash_size32) != ESP_OK) return BRUCE_ERR_IO;
    s_flash_size = flash_size32;

    uint64_t static_end = 0;
    esp_partition_iterator_t iterator =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        uint64_t partition_end = partition->address + partition->size;
        if (partition->flash_chip == esp_flash_default_chip && partition_end > static_end) {
            static_end = partition_end;
        }
        iterator = esp_partition_next(iterator);
    }
    s_user_area_start = partition_manager__sector_align_up(static_end);
    if (s_flash_size < SPI_FLASH_SEC_SIZE) return BRUCE_ERR_INVALID_STATE;
    s_ptable_offset = s_flash_size - SPI_FLASH_SEC_SIZE;
    if (s_user_area_start + BRUCE_PARTITION_ROOT_MIN_BYTES > s_ptable_offset) return BRUCE_ERR_INVALID_STATE;
    s_data_region_size = s_ptable_offset - s_user_area_start;

    esp_err_t ptable_err = esp_partition_register_external(
        NULL, s_ptable_offset, SPI_FLASH_SEC_SIZE, PARTITION_MANAGER__PTABLE_LABEL,
        PARTITION_MANAGER__PTABLE_TYPE, PARTITION_MANAGER__PTABLE_SUBTYPE, &s_ptable_partition
    );
    if (ptable_err != ESP_OK) {
        ESP_LOGE(TAG, "could not reserve the partition table sector: %s", esp_err_to_name(ptable_err));
        s_ptable_partition = NULL;
    }

    partition_manager__stored_header_t header;
    bool have_table = s_ptable_partition != NULL &&
                      esp_partition_read(s_ptable_partition, 0, &header, sizeof(header)) == ESP_OK &&
                      header.magic == PARTITION_MANAGER__MAGIC &&
                      header.version == PARTITION_MANAGER__VERSION &&
                      header.entry_count <= PARTITION_MANAGER__MAX_ENTRIES &&
                      partition_manager__crc32(&header) == header.crc32;

    bool format_pending[PARTITION_MANAGER__MAX_ENTRIES] = {0};
    memset(&s_boot, 0, sizeof(s_boot));
    if (have_table) {
        s_boot.count = header.entry_count;
        for (uint32_t i = 0; i < header.entry_count; ++i) {
            partition_manager__stored_entry_t *stored = &header.entries[i];
            partition_manager__entry_t *entry = &s_boot.entries[i];
            strncpy(entry->label, stored->label, BRUCE_PARTITION_LABEL_MAX - 1);
            entry->kind = (bruce_partition_kind_t)stored->kind;
            entry->offset = stored->offset;
            entry->size = stored->size;
            format_pending[i] = (stored->flags & PARTITION_MANAGER__FLAG_FORMAT_PENDING) != 0;
        }
        if (!partition_manager__table_valid(&s_boot)) {
            ESP_LOGE(TAG, "stored partition table is not usable; falling back to the default layout");
            have_table = false;
        }
    }
    if (!have_table) {
        memset(format_pending, 0, sizeof(format_pending));
        partition_manager__default_table(&s_boot);
    }

    bool applied = false;
    for (size_t i = 0; i < s_boot.count; ++i) {
        partition_manager__register_and_apply(&s_boot.entries[i], format_pending[i], &applied);
    }
    if (s_root_partition == NULL) {
        ESP_LOGE(TAG, "no usable root partition in the layout");
        return BRUCE_ERR_IO;
    }
    if (applied && have_table) {
        /* The formats are done; clear their flags so the next boot doesn't
         * redo them. Best-effort: if this write fails they merely re-run
         * (harmlessly) next boot too. */
        partition_manager__fill_header(&s_boot, &header);
        (void)partition_manager__write_header(&header);
    }

    s_committed = s_boot;
    s_working = s_boot;
    s_ready = true;
    return BRUCE_OK;
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

/* Appends one entry to a caller's output array, keeping it ordered by
 * offset. Counts what it could not fit, so a too-small buffer still reports
 * the real total. */
static void partition_manager__emit(
    bruce_partition_entry_t *entries, size_t capacity, size_t *count, const partition_manager__entry_t *source,
    bruce_partition_state_t state
) {
    size_t index = *count;
    (*count)++;
    if (entries == NULL || index >= capacity) return;

    bruce_partition_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.label, sizeof(entry.label), "%s", source->label);
    entry.kind = source->kind;
    entry.offset = source->offset;
    entry.size = source->size;
    entry.state = state;
    entry.is_root = partition_manager__is_root(source->label);

    size_t at = index;
    while (at > 0 && entries[at - 1].offset > entry.offset) {
        entries[at] = entries[at - 1];
        at--;
    }
    entries[at] = entry;
}

static bruce_result_t partition_manager__list_locked(
    const partition_manager__table_t *table, bool diff_against_boot, bruce_partition_entry_t *entries,
    size_t capacity, size_t *out_count
) {
    size_t count = 0;
    for (size_t i = 0; i < table->count; ++i) {
        const partition_manager__entry_t *entry = &table->entries[i];
        bruce_partition_state_t state = BRUCE_PARTITION_STATE_UNCHANGED;
        if (diff_against_boot) {
            if (partition_manager__find(&s_boot, entry->label) < 0) state = BRUCE_PARTITION_STATE_NEW;
            else if (partition_manager__format_pending(entry)) state = BRUCE_PARTITION_STATE_FORMAT;
        }
        partition_manager__emit(entries, capacity, &count, entry, state);
    }
    if (diff_against_boot) {
        for (size_t i = 0; i < s_boot.count; ++i) {
            if (partition_manager__find(table, s_boot.entries[i].label) >= 0) continue;
            partition_manager__emit(entries, capacity, &count, &s_boot.entries[i], BRUCE_PARTITION_STATE_DELETED);
        }
    }
    *out_count = count;
    return entries != NULL && count > capacity ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_OK;
}

bruce_result_t
partition_manager__list_current(bruce_partition_entry_t *entries, size_t capacity, size_t *out_count) {
    if (out_count == NULL || (entries == NULL && capacity != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) result = partition_manager__list_locked(&s_boot, false, entries, capacity, out_count);
    partition_manager__unlock();
    return result;
}

bruce_result_t
partition_manager__list_planned(bruce_partition_entry_t *entries, size_t capacity, size_t *out_count) {
    if (out_count == NULL || (entries == NULL && capacity != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) {
        result = partition_manager__list_locked(&s_working, true, entries, capacity, out_count);
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__status(bruce_partition_status_t *out_status) {
    if (out_status == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(out_status, 0, sizeof(*out_status));
    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) {
        uint64_t used = 0;
        for (size_t i = 0; i < s_working.count; ++i) used += s_working.entries[i].size;
        out_status->has_pending_changes = !partition_manager__tables_equal(&s_working, &s_committed);
        out_status->reboot_required = !partition_manager__tables_equal(&s_committed, &s_boot);
        out_status->total_bytes = s_data_region_size;
        out_status->used_bytes = used;
        /* The root entry always reaches up to the lowest extra partition, so
         * whatever is left over is a gap between extra partitions - space
         * only a new partition can reuse. */
        out_status->unallocated_bytes = s_data_region_size > used ? s_data_region_size - used : 0;
        out_status->max_new_size = partition_manager__max_new_size(&s_working);
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
    if ((kind == BRUCE_PARTITION_KIND_SWAP) != (strcmp(label, BRUCE_PARTITION_SWAP_LABEL) == 0)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK && s_working.count >= PARTITION_MANAGER__MAX_ENTRIES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK && partition_manager__find(&s_working, label) >= 0) result = BRUCE_ERR_ALREADY_EXISTS;
    if (result == BRUCE_OK) {
        partition_manager__span_search_t search = {.wanted = partition_manager__sector_align_up(size_bytes)};
        partition_manager__each_free_span(&s_working, partition_manager__span_visitor, &search);
        if (!search.found) {
            result = BRUCE_ERR_RESOURCE_LIMIT;
        } else {
            partition_manager__entry_t *entry = &s_working.entries[s_working.count++];
            memset(entry, 0, sizeof(*entry));
            snprintf(entry->label, sizeof(entry->label), "%s", label);
            entry->kind = kind;
            entry->offset = search.offset;
            entry->size = search.wanted;
            entry->format_explicit = true;
            partition_manager__recompute_root(&s_working);
        }
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__stage_delete(const char *label) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (label == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (partition_manager__is_root(label)) return BRUCE_ERR_PERMISSION;

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    int index = result == BRUCE_OK ? partition_manager__find(&s_working, label) : -1;
    if (result == BRUCE_OK && index < 0) result = BRUCE_ERR_NOT_FOUND;
    if (result == BRUCE_OK) {
        /* Order-preserving: the table is listed and compared in place, so a
         * delete must not shuffle the entries around it. */
        size_t tail = s_working.count - (size_t)index - 1;
        if (tail > 0) {
            memmove(
                &s_working.entries[index], &s_working.entries[index + 1],
                tail * sizeof(s_working.entries[0])
            );
        }
        s_working.count--;
        partition_manager__recompute_root(&s_working);
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__stage_format(const char *label) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (label == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    int index = result == BRUCE_OK ? partition_manager__find(&s_working, label) : -1;
    if (result == BRUCE_OK && index < 0) result = BRUCE_ERR_NOT_FOUND;
    /* A partition that does not exist yet is formatted when it is created;
     * asking for that separately means the caller is confused about what it
     * is looking at. */
    if (result == BRUCE_OK && partition_manager__find(&s_boot, label) < 0) result = BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK) s_working.entries[index].format_explicit = true;
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__commit(void) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;

    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK && !partition_manager__table_valid(&s_working)) result = BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK && s_ptable_partition == NULL) result = BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK) {
        partition_manager__stored_header_t header;
        partition_manager__fill_header(&s_working, &header);
        result = partition_manager__write_header(&header);
        if (result == BRUCE_OK) s_committed = s_working;
    }
    partition_manager__unlock();
    return result;
}

bruce_result_t partition_manager__discard(void) {
    if (!partition_manager__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    partition_manager__lock();
    bruce_result_t result = partition_manager__ensure_init_locked();
    if (result == BRUCE_OK) s_working = s_committed;
    partition_manager__unlock();
    return result;
}
