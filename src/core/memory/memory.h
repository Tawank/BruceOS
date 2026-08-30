#pragma once

#include "core_sdk/memory.h"

#include <stddef.h>

#define MEMORY__MAGIC 0x42524d31u /* "BRM1" */

typedef struct {
    uint32_t magic;
    size_t size;
    bruce_resource_id_t resource_id;
    bruce_process_id_t owner_id;
} memory__header_t;

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/memory.h"

/*
 * Core-only, object-based variants of the memory__external_*() API. These
 * exist for callers Core code that need what a raw pointer can't carry:
 * - the ELF loader's executable (XIP) mappings, which need a separate
 *   instruction-bus pointer and an explicit adopt/transfer step;
 * - callers that must know which backend an allocation landed in (e.g. to
 *   decide whether a pointer is directly CPU-writable) before deciding how
 *   to fill it.
 *
 * An object obtained here can still be freed through the public pointer
 * API: memory__external_free(data) after memory_external__map(), since both
 * paths populate the same underlying record table.
 */
bruce_result_t memory_external__alloc(
    size_t size, bool executable, bool allow_swap, bruce_memory_object_t *out_object
);
bruce_result_t
memory_external__write(const bruce_memory_object_t *object, size_t offset, const void *data, size_t size);
bruce_result_t memory_external__map(const bruce_memory_object_t *object, const void **out_data);
bruce_result_t memory_external__instruction_map(
    const bruce_memory_object_t *object, const void **out_instruction
);
bruce_result_t memory_external__adopt(bruce_memory_object_t *object);
/* Pointer-based counterpart of memory_external__adopt(), for images built
 * with the public memory__external_malloc() API. */
bruce_result_t memory_external__adopt_pointer(const void *ptr);
/* Reports which backend a memory__external_malloc()/calloc() allocation
 * landed in, for the few callers that must know before writing through the
 * pointer directly (e.g. flash-mapped swap memory isn't CPU-writable). */
bruce_result_t memory_external__backend_of(const void *ptr, bruce_memory_backend_t *out_backend);
bruce_result_t memory_external__release(bruce_memory_object_t *object);
void memory_external__get_swap_stats(size_t *out_total, size_t *out_free, size_t *out_largest);
bruce_result_t memory_external__layout(
    bruce_memory_layout_block_t *blocks, size_t capacity, size_t *out_count
);
bruce_result_t memory_external__read(uintptr_t offset, void *buffer, size_t size);
bruce_result_t memory_external__readable_size(uintptr_t offset, size_t *out_size);
