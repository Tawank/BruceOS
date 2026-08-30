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
#define MEMORY__REALLOC_CAPS MALLOC_CAP_SPIRAM
#else
#define MEMORY__MALLOC(size) malloc(size)
#define MEMORY__REALLOC_CAPS 0u
#endif

static void memory__cleanup(void *context) {
    memory__header_t *header = (memory__header_t *)context;
    /* A stale resource entry must not turn process teardown into a second
     * free. memory__free() clears the marker before releasing the allocation,
     * so a header that no longer identifies a live tracked block is ignored. */
    if (header == NULL || header->magic != MEMORY__MAGIC) return;
    header->magic = 0;
    free(header);
}

void *memory__malloc(size_t size) {
    if (size == 0 || size > SIZE_MAX - sizeof(memory__header_t)) { return NULL; }

    memory__header_t *header = MEMORY__MALLOC(sizeof(memory__header_t) + size);
    if (header == NULL) { return NULL; }

    bruce_resource_id_t resource_id = process_registry__resource_register(memory__cleanup, header);
    if (resource_id == BRUCE_RESOURCE_ID_INVALID) {
        free(header);
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

void *memory__realloc(void *ptr, size_t size) {
    if (ptr == NULL) return memory__malloc(size);
    if (size == 0) {
        memory__free(ptr);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(memory__header_t)) return NULL;

    memory__header_t *header = ((memory__header_t *)ptr) - 1;
    if (header->magic != MEMORY__MAGIC) return NULL;

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
    header->magic = 0;
    free(header);
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
