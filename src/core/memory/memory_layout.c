#include "core_sdk/memory.h"

#include <stdint.h>
#include <string.h>

#include "core/memory/memory.h"
#include "core/process/process.h"
#include "core_sdk/permission.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

typedef struct {
    bruce_memory_layout_block_t *blocks;
    size_t capacity;
    size_t count;
    bruce_memory_backend_t backend;
} memory_layout__walk_t;

typedef struct {
    uintptr_t address;
    void *buffer;
    size_t size;
    bool found;
} memory_layout__read_t;

typedef struct {
    uintptr_t address;
    size_t remaining;
    bool found;
} memory_layout__span_t;

static bruce_result_t memory_layout__require_builtin(void) {
    bool built_in = false;
    bruce_result_t result = process_registry__current_context(&built_in, NULL, 0, NULL);
    if (result != BRUCE_OK) return result;
    return built_in ? BRUCE_OK : BRUCE_ERR_PERMISSION;
}

static bruce_memory_region_t memory_layout__region(uintptr_t address) {
    const void *pointer = (const void *)address;
    if (esp_ptr_in_rtc_dram_fast(pointer)) return BRUCE_MEMORY_REGION_RTC_FAST;
    if (esp_ptr_in_diram_dram(pointer)) return BRUCE_MEMORY_REGION_DIRAM;
    if (esp_ptr_in_iram(pointer)) return BRUCE_MEMORY_REGION_IRAM;
    return BRUCE_MEMORY_REGION_DRAM;
}

static bool memory_layout__visit(
    walker_heap_into_t heap, walker_block_info_t block, void *context
) {
    memory_layout__walk_t *walk = context;
    size_t index = walk->count++;
    if (index >= walk->capacity) return true;

    bruce_memory_layout_block_t item = {
        .address = (uintptr_t)block.ptr,
        .size = block.size,
        .region_start = (uintptr_t)heap.start,
        .region_end = (uintptr_t)heap.end,
        .backend = walk->backend,
        .region = walk->backend == BRUCE_MEMORY_BACKEND_PSRAM
                      ? BRUCE_MEMORY_REGION_PSRAM
                      : memory_layout__region((uintptr_t)heap.start),
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

static bool memory_layout__read_visit(
    walker_heap_into_t heap, walker_block_info_t block, void *context
) {
    (void)heap;
    memory_layout__read_t *read = context;
    uintptr_t start = (uintptr_t)block.ptr;
    if (read->address < start || read->address - start > block.size ||
        read->size > block.size - (read->address - start)) {
        return true;
    }
    if (read->buffer != NULL) memcpy(read->buffer, (const void *)read->address, read->size);
    read->found = true;
    return false;
}

static bool memory_layout__span_visit(
    walker_heap_into_t heap, walker_block_info_t block, void *context
) {
    (void)heap;
    memory_layout__span_t *span = context;
    uintptr_t start = (uintptr_t)block.ptr;
    if (span->address < start || span->address - start >= block.size) return true;
    span->remaining = block.size - (span->address - start);
    span->found = true;
    return false;
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

bruce_result_t memory__read(
    bruce_memory_backend_t backend, uintptr_t address, void *buffer, size_t size
) {
    if (size == 0 || address > UINTPTR_MAX - size) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = memory_layout__require_builtin();
    if (result != BRUCE_OK) return result;
    if (backend == BRUCE_MEMORY_BACKEND_SWAP) {
        return memory_external__read(address, buffer, size);
    }
    uint32_t caps = backend == BRUCE_MEMORY_BACKEND_INTERNAL
                        ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
                        : backend == BRUCE_MEMORY_BACKEND_PSRAM
                              ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                              : 0;
    if (caps == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    memory_layout__read_t read = {
        .address = address,
        .buffer = buffer,
        .size = size,
    };
    heap_caps_walk(caps, memory_layout__read_visit, &read);
    return read.found ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}

bruce_result_t memory__readable_size(
    bruce_memory_backend_t backend, uintptr_t address, size_t *out_size
) {
    if (out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    bruce_result_t result = memory_layout__require_builtin();
    if (result != BRUCE_OK) return result;
    if (backend == BRUCE_MEMORY_BACKEND_SWAP) {
        return memory_external__readable_size(address, out_size);
    }
    uint32_t caps = backend == BRUCE_MEMORY_BACKEND_INTERNAL
                        ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
                        : backend == BRUCE_MEMORY_BACKEND_PSRAM
                              ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                              : 0;
    if (caps == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    memory_layout__span_t span = {.address = address};
    heap_caps_walk(caps, memory_layout__span_visit, &span);
    if (!span.found) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = span.remaining;
    return BRUCE_OK;
}
