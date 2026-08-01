#include "core_sdk/memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "core/process/process.h"
#include "core_sdk/process.h"

#define MEMORY__MAGIC 0x42524d31u /* "BRM1" */

typedef struct {
    uint32_t magic;
    size_t size;
    bruce_resource_id_t resource_id;
} memory__header_t;

static void memory__cleanup(void *context) {
    memory__header_t *header = (memory__header_t *)context;
    free(header);
}

void *memory__malloc(size_t size) {
    if (size == 0 || size > SIZE_MAX - sizeof(memory__header_t)) { return NULL; }

    memory__header_t *header = malloc(sizeof(memory__header_t) + size);
    if (header == NULL) { return NULL; }

    bruce_resource_id_t resource_id = process_registry__resource_register(memory__cleanup, header);
    if (resource_id == BRUCE_RESOURCE_ID_INVALID) {
        free(header);
        return NULL;
    }

    header->magic = MEMORY__MAGIC;
    header->size = size;
    header->resource_id = resource_id;
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
    memory__header_t *grown = process_registry__resource_realloc(resource_id, header, sizeof(*grown) + size);
    if (grown == NULL) return NULL;
    grown->magic = MEMORY__MAGIC;
    grown->size = size;
    grown->resource_id = resource_id;
    process_registry__account_memory((int64_t)size - (int64_t)old_size);
    return grown + 1;
}

void memory__free(void *ptr) {
    if (ptr == NULL) { return; }
    memory__header_t *header = ((memory__header_t *)ptr) - 1;
    if (header->magic != MEMORY__MAGIC) { return; }
    process_registry__resource_release(header->resource_id);
    process_registry__account_memory(-(int64_t)header->size);
    header->magic = 0;
    free(header);
}

bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats) {
    if (out_stats == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_stats = (bruce_memory_stats_t){
        .internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
        .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .internal_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        .psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        .psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .psram_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
    };
    return BRUCE_OK;
}
