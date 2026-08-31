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
/* Slots subdivide one 64KB swap page into MEMORY_EXTERNAL__FLASH_SECTOR-sized
 * (4KB) pieces so several small, non-executable, same-process allocations can
 * share one physical page instead of each rounding up to a whole one. See the
 * comment on memory_external__allocate_slot_locked() for why sector alignment
 * and single-process ownership are load-bearing, not just tidy defaults. */
#define MEMORY_EXTERNAL__SLOTS_PER_PAGE (MEMORY_EXTERNAL__MMU_PAGE / MEMORY_EXTERNAL__FLASH_SECTOR)
#define MEMORY_EXTERNAL__SLAB_THRESHOLD (MEMORY_EXTERNAL__MMU_PAGE / 4)

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
    bool is_slot;
    size_t slot_page;
    size_t slot_first;
    size_t slot_count;
} memory_external__record_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static memory_external__record_t s_records[MEMORY_EXTERNAL__MAX_OBJECTS];
static bool s_pages[MEMORY_EXTERNAL__MAX_PAGES];
static size_t s_next_page;
static uint32_t s_next_handle = 1;

/* Slab-page bookkeeping. A page becomes a slab page the first time a small
 * object is allocated into it; s_slots[page] tracks which of its 4KB slots
 * are occupied, s_slot_owner[page] is the one process allowed to place new
 * slots there, and s_page_mmap[page]/s_page_data[page] are the single shared
 * esp_partition_mmap() covering the whole page (mapped once, not once per
 * slot -- see memory_external__allocate_slot_locked()). Only meaningful
 * while s_pages[page] && s_page_is_slab[page]. */
static bool s_page_is_slab[MEMORY_EXTERNAL__MAX_PAGES];
static bool s_slots[MEMORY_EXTERNAL__MAX_PAGES][MEMORY_EXTERNAL__SLOTS_PER_PAGE];
static bruce_process_id_t s_slot_owner[MEMORY_EXTERNAL__MAX_PAGES];
static esp_partition_mmap_handle_t s_page_mmap[MEMORY_EXTERNAL__MAX_PAGES];
static const void *s_page_data[MEMORY_EXTERNAL__MAX_PAGES];

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

/* Looks up the record whose payload pointer is exactly `ptr`, as returned by
 * memory__external_malloc()/calloc(). The public pointer-based API has no
 * handle to look up by, so it identifies records by pointer identity
 * instead; ownership is checked separately by each caller. */
static memory_external__record_t *memory_external__record_at_locked(const void *ptr) {
    if (ptr == NULL) return NULL;
    for (size_t i = 0; i < MEMORY_EXTERNAL__MAX_OBJECTS; ++i) {
        memory_external__record_t *record = &s_records[i];
        if (record->backend != BRUCE_MEMORY_BACKEND_INVALID && record->data == ptr) return record;
    }
    return NULL;
}

static bruce_result_t memory_external__object_for_pointer(const void *ptr, bruce_memory_object_t *out_object) {
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__record_at_locked(ptr);
    if (record != NULL) {
        *out_object = (bruce_memory_object_t){
            .handle = record->handle,
            .size = record->size,
            .backend = record->backend,
        };
    }
    xSemaphoreGive(s_mutex);
    return record != NULL ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}

static void memory_external__cleanup(void *context) {
    memory_external__record_t *record = context;
    if (record == NULL || record->backend == BRUCE_MEMORY_BACKEND_INVALID) return;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (record->backend == BRUCE_MEMORY_BACKEND_PSRAM || record->backend == BRUCE_MEMORY_BACKEND_INTERNAL) {
        heap_caps_free(record->psram);
    } else if (record->is_slot) {
        /* Release just this record's slots. The page's shared mmap and
         * s_pages[]/s_page_is_slab[] entry only go away once every slot in
         * it is empty -- normally driven by one pass of process exit cleanup
         * over all of that process's records, since a slab page only ever
         * hosts slots from the one process it was assigned to. */
        size_t page = record->slot_page;
        for (size_t i = 0; i < record->slot_count; ++i) s_slots[page][record->slot_first + i] = false;
        bool empty = true;
        for (size_t i = 0; empty && i < MEMORY_EXTERNAL__SLOTS_PER_PAGE; ++i) empty = !s_slots[page][i];
        if (empty) {
            esp_partition_munmap(s_page_mmap[page]);
            s_pages[page] = false;
            s_page_is_slab[page] = false;
            s_slot_owner[page] = BRUCE_PROCESS_ID_INVALID;
        }
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
 * physical page's *actual registered* MMU mapping (see
 * spi_flash_check_and_flush_cache() / is_page_mapped_in_cache() in IDF's
 * flash_mmap.c, which resolve the mapping via esp_mmu_paddr_find_caps() and
 * pick the instruction- or data-bus vaddr based on whichever caps that
 * mapping was actually registered with).
 *
 * For a non-executable record, that's exactly record->data: it's the primary
 * ESP_PARTITION_MMAP_DATA mapping IDF itself tracks, so esp_partition_write()
 * already invalidates it correctly on its own.
 *
 * For an executable record, it isn't: record->data there is a *second*,
 * manually-derived data-bus alias of the same physical page
 * (spi_flash_phys2cache() with SPI_FLASH_MMAP_DATA) sitting alongside the
 * ESP_PARTITION_MMAP_INST mapping IDF actually tracks for that address.
 * esp_mmu_paddr_find_caps() only ever finds the one mapping it knows about
 * (the instruction one, since that's what execution needs), so
 * esp_partition_write()'s own invalidation only ever touches the
 * instruction-bus line -- this second, IDF-invisible data-bus alias is never
 * invalidated on write, and a read through it (including the direct_write
 * check below) can keep returning pre-write bytes indefinitely.
 *
 * IDF's own invalidation is therefore provably sufficient for the
 * non-executable case and strictly necessary only for the executable one --
 * but this is applied to every swap-backed record unconditionally anyway
 * (record->data always denotes *some* data-bus alias, IDF-tracked or not):
 * it's a few extra, always-safe esp_cache_msync() calls, and it means this
 * function doesn't have to stay in lockstep with any future change to how
 * IDF resolves esp_mmu_paddr_find_caps() for a plain MMAP_DATA mapping.
 * Called once right after allocating (a fresh mapping can recycle a virtual
 * address a previous, already-torn-down record left cached) and again after
 * every write/fill.
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

static void
memory_external__invalidate_data_alias(const memory_external__record_t *record, size_t offset, size_t size) {
    if (record->data == NULL || size == 0) return;
    uintptr_t start = (uintptr_t)(record->data + offset);
    uintptr_t end =
        (start + size + MEMORY_EXTERNAL__CACHE_ALIGN - 1) & ~(uintptr_t)(MEMORY_EXTERNAL__CACHE_ALIGN - 1);
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

static bool memory_external__allocate_internal_locked(memory_external__record_t *record, size_t size) {
    void *data = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (data == NULL) return false;
    record->backend = BRUCE_MEMORY_BACKEND_INTERNAL;
    record->size = size;
    record->psram = data;
    record->data = data;
    return true;
}

/* Scans one slab page's slot bitmap for `wanted` contiguous free slots.
 * Caller holds s_mutex. */
static bool
memory_external__page_has_room_locked(size_t page, size_t wanted, size_t *out_first) {
    for (size_t start = 0; start + wanted <= MEMORY_EXTERNAL__SLOTS_PER_PAGE; ++start) {
        bool available = true;
        for (size_t i = 0; available && i < wanted; ++i) available = !s_slots[page][start + i];
        if (available) {
            *out_first = start;
            return true;
        }
    }
    return false;
}

/*
 * Sub-allocates a small, non-executable object into a 4KB-aligned slot
 * within a 64KB swap page shared with other objects -- unlike
 * memory_external__allocate_page_locked(), which gives every object a whole
 * page to itself. Two constraints here are load-bearing, not stylistic:
 *
 * 1. Slots are always MEMORY_EXTERNAL__FLASH_SECTOR-aligned and sized in
 *    whole sectors. Flash can only be erased at sector granularity, and
 *    memory_external__store_locked()'s read-modify-erase-write path erases
 *    and rewrites the *whole* sector containing a write. Keeping every
 *    slot's byte range wholly inside its own sector(s) means that erase can
 *    never reach into a neighboring object's bytes -- sector exclusivity is
 *    what makes sharing a page safe, not any locking around the erase.
 *
 * 2. A slab page is only ever handed out to slots owned by the ONE process
 *    that first claimed it (s_slot_owner[page]); a second process asking for
 *    a small allocation gets its own page (or a slab page it already owns),
 *    never a slot on someone else's. Reads of a mapped record are always
 *    lock-free raw pointer dereferences (see memory__external_malloc()), so
 *    nothing serializes a read against a write to a *different* record --
 *    that's fine when every record's owner is also the only process that
 *    ever reads or writes it (BruceOS's existing ownership contract already
 *    requires this: writes/frees are rejected for a non-owner, and
 *    memory_external__adopt() is the only sanctioned handoff, after which
 *    the previous owner is expected to stop touching the pointer). Sharing
 *    a page across *different* processes' concurrently-live objects would
 *    reopen exactly that hazard for two objects that never agreed to
 *    synchronize with each other; keeping a slab page single-process avoids
 *    it by construction instead of relying on cache-invalidation timing.
 *
 * A page's mmap is created once, when it first becomes a slab page, and
 * shared by every slot placed in it afterward (s_page_mmap/s_page_data) --
 * never re-mapped per slot. A second esp_partition_mmap() of an
 * already-mapped physical range doesn't create an independent mapping (IDF
 * detects the shared physical range and hands back the existing one), which
 * is exactly the mechanism that produced a LoadStoreError crash the last
 * time this file mapped the same physical page twice for two different
 * capability sets (see the executable/data-alias handling below).
 */
static bool memory_external__allocate_slot_locked(
    memory_external__record_t *record, size_t size, bruce_process_id_t owner_id
) {
    const esp_partition_t *partition = memory_external__partition();
    if (partition == NULL || partition->size % MEMORY_EXTERNAL__MMU_PAGE != 0) return false;
    size_t total_pages = partition->size / MEMORY_EXTERNAL__MMU_PAGE;
    if (total_pages > MEMORY_EXTERNAL__MAX_PAGES) return false;
    size_t wanted = (size + MEMORY_EXTERNAL__FLASH_SECTOR - 1u) / MEMORY_EXTERNAL__FLASH_SECTOR;

    size_t page = SIZE_MAX;
    size_t first_slot = 0;
    for (size_t p = 0; p < total_pages; ++p) {
        if (s_pages[p] && s_page_is_slab[p] && s_slot_owner[p] == owner_id &&
            memory_external__page_has_room_locked(p, wanted, &first_slot)) {
            page = p;
            break;
        }
    }

    bool fresh_page = false;
    if (page == SIZE_MAX) {
        for (size_t pass = 0; pass < total_pages; ++pass) {
            size_t candidate = (s_next_page + pass) % total_pages;
            if (!s_pages[candidate]) {
                page = candidate;
                break;
            }
        }
        if (page == SIZE_MAX) return false;
        first_slot = 0;
        fresh_page = true;
    }

    if (fresh_page) {
        const void *mapped = NULL;
        esp_partition_mmap_handle_t mmap_handle;
        if (esp_partition_mmap(
                partition, page * MEMORY_EXTERNAL__MMU_PAGE, MEMORY_EXTERNAL__MMU_PAGE, ESP_PARTITION_MMAP_DATA,
                &mapped, &mmap_handle
            ) != ESP_OK) {
            return false;
        }
        memset(s_slots[page], 0, sizeof(s_slots[page]));
        s_pages[page] = true;
        s_page_is_slab[page] = true;
        s_slot_owner[page] = owner_id;
        s_page_mmap[page] = mmap_handle;
        s_page_data[page] = mapped;
        s_next_page = (page + 1) % total_pages;
    }

    size_t offset = page * MEMORY_EXTERNAL__MMU_PAGE + first_slot * MEMORY_EXTERNAL__FLASH_SECTOR;
    if (esp_partition_erase_range(partition, offset, wanted * MEMORY_EXTERNAL__FLASH_SECTOR) != ESP_OK) {
        if (fresh_page) {
            esp_partition_munmap(s_page_mmap[page]);
            s_pages[page] = false;
            s_page_is_slab[page] = false;
            s_slot_owner[page] = BRUCE_PROCESS_ID_INVALID;
        }
        return false;
    }

    for (size_t i = 0; i < wanted; ++i) s_slots[page][first_slot + i] = true;
    record->backend = BRUCE_MEMORY_BACKEND_SWAP;
    record->size = size;
    record->offset = offset;
    record->executable = false;
    record->is_slot = true;
    record->slot_page = page;
    record->slot_first = first_slot;
    record->slot_count = wanted;
    record->data = (const uint8_t *)s_page_data[page] + first_slot * MEMORY_EXTERNAL__FLASH_SECTOR;
    record->instruction = NULL;
    /* See memory_external__invalidate_data_alias() above: cheap, always-safe
     * defense-in-depth, doubly worthwhile here since this slot's virtual
     * address range may have hosted a different, already-freed object. */
    memory_external__invalidate_data_alias(record, 0, record->size);
    return true;
}

static bool
memory_external__allocate_page_locked(memory_external__record_t *record, size_t size, bool executable) {
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
    record->is_slot = false;
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
    } else {
        record->data = mapped;
    }
    /* See the comment on memory_external__invalidate_data_alias() above:
     * strictly necessary only for the executable case, applied to both for
     * symmetry and defense-in-depth. */
    memory_external__invalidate_data_alias(record, 0, record->size);
    return true;
}

/* Small (< MEMORY_EXTERNAL__SLAB_THRESHOLD), non-executable requests try to
 * share a slot in one of the requesting process's own slab pages first,
 * falling back to a whole page (as every request always did before slabs
 * existed) only if no slot placement succeeds -- e.g. every page this
 * process already owns is full and no fresh page is free either, in which
 * case memory_external__allocate_page_locked() will independently fail too
 * and the caller sees the same BRUCE_ERR_RESOURCE_LIMIT it always would
 * have. Executable objects and anything at or above the threshold skip
 * straight to the whole-page path. */
static bool memory_external__allocate_swap_locked(
    memory_external__record_t *record, size_t size, bool executable, bruce_process_id_t owner_id
) {
    if (!executable && size < MEMORY_EXTERNAL__SLAB_THRESHOLD &&
        memory_external__allocate_slot_locked(record, size, owner_id)) {
        return true;
    }
    return memory_external__allocate_page_locked(record, size, executable);
}

bruce_result_t
memory_external__alloc(size_t size, bool executable, bool allow_swap, bruce_memory_object_t *out_object) {
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
    if (record != NULL && !allocated && allow_swap) {
        allocated = memory_external__allocate_swap_locked(record, size, executable, owner_id);
    }
    if (record != NULL && !allocated && !executable) {
        allocated = memory_external__allocate_internal_locked(record, size);
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

/*
 * Shared byte-store logic for both memory_external__write() (copies from a
 * caller buffer) and memory_external__fill() (repeats one byte value), used
 * by the public memcpy()/memset() functions. Caller holds s_mutex and has
 * already validated the record/offset/size against `object`.
 */
static bruce_result_t memory_external__store_locked(
    memory_external__record_t *record, size_t offset, const void *data, int fill_value, bool is_fill, size_t size
) {
    if (offset > record->size || size > record->size - offset) return BRUCE_ERR_INVALID_ARGUMENT;
    if (size == 0) return BRUCE_OK;

    if (record->backend == BRUCE_MEMORY_BACKEND_PSRAM || record->backend == BRUCE_MEMORY_BACKEND_INTERNAL) {
        if (is_fill) memset((uint8_t *)record->psram + offset, fill_value, size);
        else memmove((uint8_t *)record->psram + offset, data, size);
        return BRUCE_OK;
    }

    const uint8_t *bytes = data;
    if (!is_fill) {
        uintptr_t source_start = (uintptr_t)data;
        uintptr_t source_end = source_start + size;
        uintptr_t mapping_start = (uintptr_t)record->data;
        uintptr_t mapping_end = mapping_start + record->size;
        if (source_end < source_start || mapping_end < mapping_start ||
            (source_start < mapping_end && source_end > mapping_start)) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    }

    const esp_partition_t *partition = memory_external__partition();
    if (partition == NULL) return BRUCE_ERR_IO;

    /* Flash operations disable the cache. Caller data may live in a flash
     * mmap (external-object growth does exactly that), and a plain malloc
     * buffer is not an explicit cache-safe contract. Stage every operation
     * through internal RAM so neither the partition driver nor its ROM
     * memcpy touches cached memory while the cache is disabled. */
    uint8_t *sector = heap_caps_malloc(MEMORY_EXTERNAL__FLASH_SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (sector == NULL) return BRUCE_ERR_NO_MEMORY;

    bruce_result_t result = BRUCE_OK;
    size_t written = 0;
    while (result == BRUCE_OK && written < size) {
        size_t absolute = record->offset + offset + written;
        size_t sector_offset = absolute & ~(MEMORY_EXTERNAL__FLASH_SECTOR - 1u);
        size_t in_sector = absolute - sector_offset;
        size_t chunk = size - written;
        if (chunk > MEMORY_EXTERNAL__FLASH_SECTOR - in_sector) chunk = MEMORY_EXTERNAL__FLASH_SECTOR - in_sector;

        bool direct_write = true;
        for (size_t i = 0; i < chunk; ++i) {
            uint8_t new_byte = is_fill ? (uint8_t)fill_value : bytes[written + i];
            direct_write = direct_write && (record->data[offset + written + i] & new_byte) == new_byte;
        }

        if (direct_write) {
            if (is_fill) memset(sector, fill_value, chunk);
            else memcpy(sector, bytes + written, chunk);
            if (esp_partition_write(partition, absolute, sector, chunk) != ESP_OK) {
                result = BRUCE_ERR_IO;
                break;
            }
        } else {
            if (esp_partition_read(partition, sector_offset, sector, MEMORY_EXTERNAL__FLASH_SECTOR) != ESP_OK) {
                result = BRUCE_ERR_IO;
                break;
            }
            if (is_fill) memset(sector + in_sector, fill_value, chunk);
            else memcpy(sector + in_sector, bytes + written, chunk);
            if (esp_partition_erase_range(partition, sector_offset, MEMORY_EXTERNAL__FLASH_SECTOR) != ESP_OK ||
                esp_partition_write(partition, sector_offset, sector, MEMORY_EXTERNAL__FLASH_SECTOR) != ESP_OK) {
                result = BRUCE_ERR_IO;
                break;
            }
        }
        written += chunk;
    }
    heap_caps_free(sector);
    if (result == BRUCE_OK) memory_external__invalidate_data_alias(record, offset, size);
    return result;
}

bruce_result_t
memory_external__write(const bruce_memory_object_t *object, size_t offset, const void *data, size_t size) {
    if (object == NULL || (data == NULL && size != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    bruce_result_t result = !memory_external__matches(record, object) || record->owner_id != owner_id
                                 ? BRUCE_ERR_INVALID_ARGUMENT
                                 : memory_external__store_locked(record, offset, data, 0, false, size);
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return result;
}

static bruce_result_t
memory_external__fill(const bruce_memory_object_t *object, size_t offset, int value, size_t size) {
    if (object == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    bruce_process_id_t owner_id = process__current_id();
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_external__record_t *record = memory_external__find_locked(object->handle);
    bruce_result_t result = !memory_external__matches(record, object) || record->owner_id != owner_id
                                 ? BRUCE_ERR_INVALID_ARGUMENT
                                 : memory_external__store_locked(record, offset, NULL, value, true, size);
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return result;
}

bruce_result_t memory_external__map(const bruce_memory_object_t *object, const void **out_data) {
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

bruce_result_t memory_external__adopt_pointer(const void *ptr) {
    bruce_memory_object_t object;
    bruce_result_t result = memory_external__object_for_pointer(ptr, &object);
    if (result != BRUCE_OK) return result;
    return memory_external__adopt(&object);
}

bruce_result_t memory_external__backend_of(const void *ptr, bruce_memory_backend_t *out_backend) {
    if (out_backend == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_memory_object_t object;
    bruce_result_t result = memory_external__object_for_pointer(ptr, &object);
    if (result != BRUCE_OK) return result;
    *out_backend = object.backend;
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

const void *memory__external_malloc(size_t size) {
    if (size == 0) return NULL;
    bruce_memory_object_t object;
    if (memory_external__alloc(size, false, true, &object) != BRUCE_OK) return NULL;
    const void *data = NULL;
    if (memory_external__map(&object, &data) != BRUCE_OK) {
        (void)memory_external__release(&object);
        return NULL;
    }
    return data;
}

const void *memory__external_malloc_writable(size_t size) {
    if (size == 0) return NULL;
    bruce_memory_object_t object;
    if (memory_external__alloc(size, false, false, &object) != BRUCE_OK) return NULL;
    const void *data = NULL;
    if (memory_external__map(&object, &data) != BRUCE_OK) {
        (void)memory_external__release(&object);
        return NULL;
    }
    return data;
}

const void *memory__external_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    const void *data = memory__external_malloc(total);
    if (data != NULL && memory__external_memset(data, 0, 0, total) != BRUCE_OK) {
        (void)memory__external_free(data);
        return NULL;
    }
    return data;
}

bruce_result_t memory__external_memcpy(const void *ptr, size_t offset, const void *data, size_t size) {
    if (data == NULL && size != 0) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_memory_object_t object;
    bruce_result_t result = memory_external__object_for_pointer(ptr, &object);
    if (result != BRUCE_OK) return result;
    return memory_external__write(&object, offset, data, size);
}

bruce_result_t memory__external_memset(const void *ptr, size_t offset, int value, size_t size) {
    bruce_memory_object_t object;
    bruce_result_t result = memory_external__object_for_pointer(ptr, &object);
    if (result != BRUCE_OK) return result;
    return memory_external__fill(&object, offset, value, size);
}

bruce_result_t memory__external_free(const void *ptr) {
    if (ptr == NULL) return BRUCE_OK;
    bruce_memory_object_t object;
    bruce_result_t result = memory_external__object_for_pointer(ptr, &object);
    if (result != BRUCE_OK) return result;
    return memory_external__release(&object);
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
                /* A slab page's still-empty slots are real, allocatable
                 * bytes -- just not a contiguous run big enough for a
                 * whole-page request from a different process, so they
                 * count toward free_bytes but never toward largest. */
                if (s_page_is_slab[i]) {
                    for (size_t s = 0; s < MEMORY_EXTERNAL__SLOTS_PER_PAGE; ++s) {
                        if (!s_slots[i][s]) free_bytes += MEMORY_EXTERNAL__FLASH_SECTOR;
                    }
                }
            }
        }
    }
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    if (out_total != NULL) *out_total = total;
    if (out_free != NULL) *out_free = free_bytes;
    if (out_largest != NULL) *out_largest = largest;
}

bruce_result_t memory_external__layout(
    bruce_memory_layout_block_t *blocks, size_t capacity, size_t *out_count
) {
    if (out_count == NULL || (capacity != 0 && blocks == NULL)) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const esp_partition_t *partition = memory_external__partition();
    size_t page_count = partition == NULL ? 0 : partition->size / MEMORY_EXTERNAL__MMU_PAGE;
    for (size_t page = 0; page < page_count;) {
        if (s_pages[page] && s_page_is_slab[page]) {
            /* Slab page: report one block per live slot run (and per free
             * slot run in between), instead of one block for the whole
             * page -- a slab page can hold several different owners' small
             * objects at once. */
            for (size_t slot = 0; slot < MEMORY_EXTERNAL__SLOTS_PER_PAGE;) {
                const memory_external__record_t *owner = NULL;
                for (size_t i = 0; i < MEMORY_EXTERNAL__MAX_OBJECTS; ++i) {
                    if (s_records[i].backend == BRUCE_MEMORY_BACKEND_SWAP && s_records[i].is_slot &&
                        s_records[i].slot_page == page && s_records[i].slot_first == slot) {
                        owner = &s_records[i];
                        break;
                    }
                }
                size_t slots = 1;
                if (owner != NULL) {
                    slots = owner->slot_count;
                } else {
                    while (slot + slots < MEMORY_EXTERNAL__SLOTS_PER_PAGE && !s_slots[page][slot + slots]) ++slots;
                }
                size_t index = (*out_count)++;
                if (index < capacity) {
                    blocks[index] = (bruce_memory_layout_block_t){
                        .address = page * MEMORY_EXTERNAL__MMU_PAGE + slot * MEMORY_EXTERNAL__FLASH_SECTOR,
                        .size = slots * MEMORY_EXTERNAL__FLASH_SECTOR,
                        .region_start = 0,
                        .region_end = partition->size,
                        .requested_size = owner == NULL ? 0 : owner->size,
                        .backend = BRUCE_MEMORY_BACKEND_SWAP,
                        .region = BRUCE_MEMORY_REGION_SWAP,
                        .owner_id = owner == NULL ? BRUCE_PROCESS_ID_INVALID : owner->owner_id,
                        .handle = owner == NULL ? 0 : owner->handle,
                        .used = owner != NULL,
                        .tracked = owner != NULL,
                        .executable = false,
                    };
                }
                slot += slots;
            }
            ++page;
            continue;
        }

        const memory_external__record_t *owner = NULL;
        for (size_t i = 0; i < MEMORY_EXTERNAL__MAX_OBJECTS; ++i) {
            if (s_records[i].backend == BRUCE_MEMORY_BACKEND_SWAP && !s_records[i].is_slot &&
                s_records[i].offset / MEMORY_EXTERNAL__MMU_PAGE == page) {
                owner = &s_records[i];
                break;
            }
        }
        size_t pages = 1;
        if (owner != NULL) {
            pages = owner->page_count;
        } else {
            while (page + pages < page_count && !s_pages[page + pages]) ++pages;
        }
        size_t index = (*out_count)++;
        if (index < capacity) {
            blocks[index] = (bruce_memory_layout_block_t){
                .address = page * MEMORY_EXTERNAL__MMU_PAGE,
                .size = pages * MEMORY_EXTERNAL__MMU_PAGE,
                .region_start = 0,
                .region_end = partition->size,
                .requested_size = owner == NULL ? 0 : owner->size,
                .backend = BRUCE_MEMORY_BACKEND_SWAP,
                .region = BRUCE_MEMORY_REGION_SWAP,
                .owner_id = owner == NULL ? BRUCE_PROCESS_ID_INVALID : owner->owner_id,
                .handle = owner == NULL ? 0 : owner->handle,
                .used = owner != NULL,
                .tracked = owner != NULL,
                .executable = owner != NULL && owner->executable,
            };
        }
        page += pages;
    }
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return BRUCE_OK;
}

bruce_result_t memory_external__read(uintptr_t offset, void *buffer, size_t size) {
    if (size == 0 || offset > SIZE_MAX - size) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const memory_external__record_t *record = NULL;
    for (size_t i = 0; i < MEMORY_EXTERNAL__MAX_OBJECTS; ++i) {
        const memory_external__record_t *candidate = &s_records[i];
        if (candidate->backend != BRUCE_MEMORY_BACKEND_SWAP || offset < candidate->offset) continue;
        size_t relative = offset - candidate->offset;
        if (relative <= candidate->size && size <= candidate->size - relative) {
            record = candidate;
            break;
        }
    }
    if (record != NULL && buffer != NULL) memcpy(buffer, record->data + (offset - record->offset), size);
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return record != NULL ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}

bruce_result_t memory_external__readable_size(uintptr_t offset, size_t *out_size) {
    if (out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    memory_external__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const memory_external__record_t *record = NULL;
    for (size_t i = 0; i < MEMORY_EXTERNAL__MAX_OBJECTS; ++i) {
        const memory_external__record_t *candidate = &s_records[i];
        if (candidate->backend != BRUCE_MEMORY_BACKEND_SWAP || offset < candidate->offset) continue;
        size_t relative = offset - candidate->offset;
        if (relative < candidate->size) {
            record = candidate;
            *out_size = candidate->size - relative;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    process_registry__operation_end();
    return record != NULL ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}
