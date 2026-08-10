#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

typedef struct {
    size_t internal_total;
    size_t internal_free;
    size_t internal_largest_block;
    size_t internal_minimum_free;
    size_t internal_free_blocks;
    size_t psram_total;
    size_t psram_free;
    size_t psram_largest_block;
    size_t swap_total;
    size_t swap_free;
    size_t swap_largest_block;
} bruce_memory_stats_t;

typedef enum {
    BRUCE_MEMORY_BACKEND_INVALID = 0,
    BRUCE_MEMORY_BACKEND_PSRAM,
    BRUCE_MEMORY_BACKEND_SWAP,
    BRUCE_MEMORY_BACKEND_INTERNAL,
} bruce_memory_backend_t;

typedef struct {
    uint32_t handle;
    size_t size;
    bruce_memory_backend_t backend;
} bruce_memory_object_t;

/*
 * Process-owned tracked heap allocator.
 *
 * ELF apps never receive libc malloc/free (those imports are rejected by the
 * loader); JS and built-in code should also prefer this allocator so memory
 * is accounted against the owning process and released automatically if the
 * process exits or is killed without freeing it first.
 *
 * memory__malloc() must be called from within a Core-managed process (i.e. a
 * process started by process_registry__create()/AppRunner); calling it with no
 * current process returns NULL, the same result as an allocation failure.
 */
void *memory__malloc(size_t size);

/* Allocates and zero-initializes count objects of size bytes. Returns NULL
 * when either argument is zero or their product overflows size_t. */
void *memory__calloc(size_t count, size_t size);

/* Resizes a tracked allocation while preserving its process ownership. The
 * libc realloc rules apply for NULL ptr and zero size. */
void *memory__realloc(void *ptr, size_t size);

/* Frees memory obtained from memory__malloc()/memory__calloc()/
 * memory__realloc(). NULL is a no-op. Passing any other pointer, or one
 * already freed, is undefined behaviour, matching libc free(). */
void memory__free(void *ptr);

/* Allocates a process-owned external-memory object. PSRAM is preferred when a
 * sufficiently large block is available; otherwise complete 64 KiB pages are
 * allocated from swap. If neither is available -- no PSRAM on this board, and
 * no "swap" partition has been committed yet (see partitions.csv) -- plain
 * internal RAM is used as a last resort so callers of this function still get
 * a usable object instead of BRUCE_ERR_RESOURCE_LIMIT.
 * The returned object is released automatically when its process exits. */
bruce_result_t memory__external_alloc(size_t size, bruce_memory_object_t *out_object);

/* Writes or replaces bytes through the object's backend without exposing a
 * writable flash pointer. Swap updates that require changing a zero bit back
 * to one rewrite the affected flash sectors. Callers must synchronize writes
 * with readers of an already shared mapping. */
bruce_result_t
memory__external_write(const bruce_memory_object_t *object, size_t offset, const void *data, size_t size);

/* Returns a read-only mapping that remains valid until memory__external_free()
 * or process teardown. */
bruce_result_t memory__external_map(const bruce_memory_object_t *object, const void **out_data);

/* Releases an external object. NULL is invalid; a successful call clears the
 * caller's object. Payload bytes are not zeroed. */
bruce_result_t memory__external_free(bruce_memory_object_t *object);

/* Returns current heap capacity plus fragmentation/high-water diagnostics.
 * `internal_free_blocks` counts currently free allocator blocks across all
 * internal-capable heap regions; it includes the regions' natural boundaries. */
bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats);
