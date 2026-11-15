#include "core_sdk/memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "core/process/process.h"
#include "core/memory/memory.h"
#include "core_sdk/process.h"

#include "sdkconfig.h"

#if CONFIG_BRUCE_MEMORY_FORCE_PSRAM
/* See BRUCE_MEMORY_FORCE_PSRAM's Kconfig help text: with this on, every
 * memory__malloc()/calloc()/realloc() allocation must land in PSRAM, with no
 * silent internal-RAM fallback - so a full PSRAM heap fails the allocation
 * (NULL) rather than eating into the internal RAM this option exists to
 * protect. */
#define MEMORY__MALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM)
#define MEMORY__ALIGNED_MALLOC(size) heap_caps_aligned_alloc(MEMORY__ALIGNMENT, (size), MALLOC_CAP_SPIRAM)
#define MEMORY__REALLOC_CAPS MALLOC_CAP_SPIRAM
#else
#define MEMORY__MALLOC(size) malloc(size)
#define MEMORY__ALIGNED_MALLOC(size) heap_caps_aligned_alloc(MEMORY__ALIGNMENT, (size), MALLOC_CAP_DEFAULT)
#define MEMORY__REALLOC_CAPS 0u
#endif

static bool memory__wants_alignment(size_t size) { return size >= MEMORY__ALIGNED_MIN_SIZE; }

static bool memory__is_aligned(const void *ptr) { return ((uintptr_t)ptr & (MEMORY__ALIGNMENT - 1)) == 0; }

static void memory__release_block(memory__header_t *header) {
    header->magic = 0;
    if (memory_rtc__contains(header)) {
        memory_rtc__free(header);
    } else {
        free(header);
    }
}

static void memory__cleanup(void *context) {
    memory__header_t *header = (memory__header_t *)context;
    /* A stale resource entry must not turn process teardown into a second
     * free. memory__free() clears the marker before releasing the allocation,
     * so a header that no longer identifies a live tracked block is ignored. */
    if (header == NULL || header->magic != MEMORY__MAGIC) return;
    memory__release_block(header);
}

void *memory__malloc(size_t size) {
    if (size == 0 || size > UINT32_MAX - sizeof(memory__header_t)) { return NULL; }
    size_t total = sizeof(memory__header_t) + size;

    /* Small allocations get first crack at RTC memory - both the hand-rolled
     * RTC_SLOW pool (memory_rtc.c) and, straight through esp_heap_caps,
     * RTC_FAST - before falling back to the general internal/PSRAM heap.
     * Neither is memory a plain malloc() ever reaches in practice (RTC_SLOW
     * has no heap_caps region at all; RTC_FAST matches MALLOC_CAP_DEFAULT at
     * LOW priority behind D/IRAM and DRAM, so it's only ever reached once
     * both of those are full - see memory_rtc.c's header comment), so
     * steering small, short-lived allocations there first is what keeps the
     * big D/IRAM region's largest free block from fragmenting under them.
     *
     * Skipped entirely under CONFIG_BRUCE_MEMORY_FORCE_PSRAM: that option's
     * whole point is routing memory__malloc() traffic away from internal RAM
     * (see its Kconfig help text), and both RTC regions are internal RAM -
     * silently landing allocations there would defeat it exactly the way it
     * exists to prevent.
     *
     * Buffers big enough to be worth vectorising skip the RTC pool (it only
     * guarantees 4-byte alignment) and come from aligned heap allocations -
     * see MEMORY__ALIGNED_MIN_SIZE. */
    bool aligned = memory__wants_alignment(size);
    memory__header_t *header = NULL;
#if !CONFIG_BRUCE_MEMORY_FORCE_PSRAM
    if (aligned) {
        header = heap_caps_aligned_alloc(MEMORY__ALIGNMENT, total, MALLOC_CAP_RTCRAM);
    } else {
        header = memory_rtc__alloc(total);
        if (header == NULL) { header = heap_caps_malloc(total, MALLOC_CAP_RTCRAM); }
    }
#endif
    if (header == NULL) { header = aligned ? MEMORY__ALIGNED_MALLOC(total) : MEMORY__MALLOC(total); }
    if (header == NULL) { return NULL; }

    bruce_resource_id_t resource_id = process_registry__resource_register(memory__cleanup, header);
    if (resource_id == BRUCE_RESOURCE_ID_INVALID) {
        memory__release_block(header);
        return NULL;
    }

    header->magic = MEMORY__MAGIC;
    header->size = size;
    header->resource_id = resource_id;
    header->owner_id = process__current_id();
    process_registry__account_memory((int64_t)size);
    return (void *)(header + 1);
}

void *memory__calloc(size_t count, size_t size) {
    if (count == 0 || size == 0 || count > SIZE_MAX / size) { return NULL; }
    size_t total = count * size;
    void *ptr = memory__malloc(total);
    if (ptr != NULL) { memset(ptr, 0, total); }
    return ptr;
}

static void *memory__migrate(void *ptr, size_t size) {
    const memory__header_t *header = ((const memory__header_t *)ptr) - 1;
    void *migrated = memory__malloc(size);
    if (migrated == NULL) return NULL;
    memcpy(migrated, ptr, header->size < size ? header->size : size);
    memory__free(ptr);
    return migrated;
}

void *memory__realloc(void *ptr, size_t size) {
    if (ptr == NULL) return memory__malloc(size);
    if (size == 0) {
        memory__free(ptr);
        return NULL;
    }
    if (size > UINT32_MAX - sizeof(memory__header_t)) return NULL;

    memory__header_t *header = ((memory__header_t *)ptr) - 1;
    if (header->magic != MEMORY__MAGIC) return NULL;

    if (memory_rtc__contains(header)) {
        /* The RTC pool has no in-place grow/shrink (see memory_rtc.c) -
         * migrate by allocating fresh, which gets its own shot at the pool
         * and falls back to the heap exactly like any other memory__malloc()
         * call, rather than handing a non-heap pointer to
         * process_registry__resource_realloc() below. */
        return memory__migrate(ptr, size);
    }

    size_t old_size = header->size;
    bruce_resource_id_t resource_id = header->resource_id;
    memory__header_t *grown = process_registry__resource_realloc(
        resource_id, header, sizeof(*grown) + size, MEMORY__REALLOC_CAPS
    );
    if (grown == NULL) return NULL;
    grown->magic = MEMORY__MAGIC;
    grown->size = size;
    grown->resource_id = resource_id;
    /* owner_id is preserved by realloc; resources cannot change owner here. */
    process_registry__account_memory((int64_t)size - (int64_t)old_size);
    /* Plain realloc() keeps the block in place when it can, which keeps an
     * aligned block aligned; when it moved or grew a small block past the
     * threshold, one more copy puts it where memory__malloc() would have.
     * The resize already succeeded, so a failed copy still returns it. */
    if (memory__wants_alignment(size) && !memory__is_aligned(grown + 1)) {
        void *migrated = memory__migrate(grown + 1, size);
        if (migrated != NULL) return migrated;
    }
    return grown + 1;
}

void memory__free(void *ptr) {
    if (ptr == NULL) { return; }
    memory__header_t *header = ((memory__header_t *)ptr) - 1;
    if (header->magic != MEMORY__MAGIC) { return; }
    /* Only free a tracked block after removing this exact header from the
     * calling process's resource list. If its metadata is stale/corrupted, or
     * it is freed from the wrong process, leave the registered allocation for
     * its owner's teardown instead of freeing it while a cleanup entry still
     * points at it (which would become a teardown-time double free). */
    if (process_registry__resource_release_exact(header->resource_id, header) != BRUCE_OK) return;
    process_registry__account_memory(-(int64_t)header->size);
    memory__release_block(header);
}

void *memory__stack_alloc(size_t total_bytes) {
    /* RTC_FAST only, deliberately never memory_rtc.c's RTC_SLOW pool:
     * esp_stack_ptr_is_sane() (esp_memory_utils.h) - which FreeRTOS/the IDF
     * port use to validate a stack pointer on every context switch - accepts
     * DRAM, ext RAM, and RTC_FAST, but never RTC_SLOW under any Kconfig
     * option on this chip. Confirmed the hard way: a stack landed in the
     * RTC_SLOW pool boot-loops at the very first context switch. RTC_FAST
     * rarely has room in practice (a default-sized stack is already half its
     * ~8KB region, and the 8KB GUI tier doesn't fit at all), so most stacks
     * fall straight through to the internal heap below. Stacks must never use
     * PSRAM: flash writes disable caches, and ESP-IDF asserts if the current
     * task stack is external while that happens. */
    void *ptr = heap_caps_malloc(total_bytes, MALLOC_CAP_RTCRAM);
    if (ptr == NULL) { ptr = heap_caps_malloc(total_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
    return ptr;
}

void memory__stack_free(void *ptr) {
    /* Both sources memory__stack_alloc() can return - RTC_FAST or internal
     * DRAM via heap_caps_malloc() - are freed identically. */
    free(ptr);
}

bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats) {
    if (out_stats == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    multi_heap_info_t internal_info;
    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);
    *out_stats = (bruce_memory_stats_t){
        .internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
        .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .internal_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        .internal_minimum_free = internal_info.minimum_free_bytes,
        .internal_free_blocks = internal_info.free_blocks,
        .psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        .psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .psram_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
    };
    memory_external__get_swap_stats(
        &out_stats->swap_total, &out_stats->swap_free, &out_stats->swap_largest_block
    );
    return BRUCE_OK;
}
