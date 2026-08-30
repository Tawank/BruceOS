#include "core_sdk/memory.h"

#include <stdint.h>

#include "core/memory/memory.h"
#include "core_sdk/permission.h"
#include "esp_heap_caps.h"

typedef struct {
    bruce_memory_layout_block_t *blocks;
    size_t capacity;
    size_t count;
    bruce_memory_backend_t backend;
} memory_layout__walk_t;

static bool memory_layout__visit(
    walker_heap_into_t heap, walker_block_info_t block, void *context
) {
    (void)heap;
    memory_layout__walk_t *walk = context;
    size_t index = walk->count++;
    if (index >= walk->capacity) return true;

    bruce_memory_layout_block_t item = {
        .address = (uintptr_t)block.ptr,
        .size = block.size,
        .backend = walk->backend,
        .owner_id = BRUCE_PROCESS_ID_INVALID,
        .used = block.used,
    };
    if (block.used && block.size >= sizeof(memory__header_t)) {
        const memory__header_t *header = block.ptr;
        if (header->magic == MEMORY__MAGIC && header->size <= block.size - sizeof(*header) &&
            header->resource_id != BRUCE_RESOURCE_ID_INVALID &&
            header->owner_id != BRUCE_PROCESS_ID_INVALID) {
            item.tracked = true;
            item.requested_size = header->size;
            item.owner_id = header->owner_id;
        }
    }
    walk->blocks[index] = item;
    return true;
}

bruce_result_t memory__get_layout(
    bruce_memory_backend_t backend, bruce_memory_layout_block_t *blocks,
    size_t capacity, size_t *out_count
) {
    if (out_count == NULL || (capacity != 0 && blocks == NULL)) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = permission__check(BRUCE_PERMISSION_PROCESS);
    if (result != BRUCE_OK) return result;
    if (backend == BRUCE_MEMORY_BACKEND_SWAP) return memory_external__layout(blocks, capacity, out_count);
    uint32_t caps = backend == BRUCE_MEMORY_BACKEND_INTERNAL
                        ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
                        : backend == BRUCE_MEMORY_BACKEND_PSRAM
                              ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                              : 0;
    if (caps == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    memory_layout__walk_t walk = {
        .blocks = blocks,
        .capacity = capacity,
        .backend = backend,
    };
    heap_caps_walk(caps, memory_layout__visit, &walk);
    *out_count = walk.count;
    return BRUCE_OK;
}
