#pragma once

#include "core_sdk/memory.h"

#include <stddef.h>

#define MEMORY__MAGIC 0x42524d31u /* "BRM1" */

typedef struct {
    uint32_t magic;
    size_t size;
    bruce_resource_id_t resource_id;
    bruce_process_id_t owner_id;
    /* See bruce_memory_layout_block_t.is_stack - carried straight through by
     * memory_layout__visit(). Always false for memory__malloc()'s own
     * headers; only process_registry__create() ever sets this true, for the
     * header it builds by hand ahead of its own xTaskCreateStatic() stack
     * buffer. */
    bool is_stack;
    /* True when this header (and its payload) live inside the small RTC
     * memory pool (see memory_rtc.c) rather than the general heap - set by
     * memory__malloc() when memory_rtc__alloc() supplied the backing memory.
     * memory__free()/memory__cleanup() check this to return the block to
     * that pool with memory_rtc__free() instead of calling free() on memory
     * libc never allocated. */
    bool is_rtc_pool;
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

/*
 * Small first-fit suballocator over a static RTC-memory pool - see
 * memory_rtc.c for what backs it and why it exists outside esp_heap_caps.
 * Returns NULL (never falls back itself) when the pool has no free chunk
 * big enough; memory__malloc() is the only caller, and it falls back to the
 * general heap itself when this returns NULL.
 */
void *memory_rtc__alloc(size_t size);
void memory_rtc__free(void *ptr);

/* One chunk (free or used) as reported by memory_rtc__walk() - address/size
 * of the payload following memory_rtc.c's internal chunk header, exactly
 * what memory_rtc__alloc() itself hands back, so a used chunk's `address`
 * can be reinterpreted by the caller as a memory__header_t* the same way a
 * general-heap block can. */
typedef struct {
    uintptr_t address;
    size_t size;
    bool used;
} memory_rtc__block_t;

/* Walks every chunk in the RTC pool in address order, holding the pool's
 * mutex for the duration, invoking visit(pool_start, pool_end, block,
 * context) for each. Deliberately shaped like esp_heap_caps' own
 * heap_caps_walk() (a bounding range plus one block at a time, and the same
 * "false return stops early" contract) without pulling esp_heap_caps.h into
 * this header - memory_layout.c (the only caller) bridges each callback into
 * the exact same per-block logic it already runs for the general heap, so
 * the RTC pool folds into memory__get_layout()/memory__read()/
 * memory__readable_size() as just another region instead of a separate,
 * invisible pool those APIs never look at. */
void memory_rtc__walk(
    bool (*visit)(uintptr_t pool_start, uintptr_t pool_end, const memory_rtc__block_t *block, void *context),
    void *context
);
