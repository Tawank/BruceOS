#pragma once

#include <stddef.h>

#include "core_sdk/result.h"

typedef struct {
    size_t internal_total;
    size_t internal_free;
    size_t internal_largest_block;
    size_t psram_total;
    size_t psram_free;
    size_t psram_largest_block;
} bruce_memory_stats_t;

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

bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats);
