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
 * Task-owned tracked heap allocator.
 *
 * ELF apps never receive libc malloc/free (those imports are rejected by the
 * loader); JS and built-in code should also prefer this allocator so memory
 * is accounted against the owning task and released automatically if the
 * task exits or is killed without freeing it first.
 *
 * memory__malloc() must be called from within a Core-managed task (i.e. a
 * task started by task_registry__create()/AppRunner); calling it with no
 * current task returns NULL, the same result as an allocation failure.
 */
void *memory__malloc(size_t size);

/* Frees memory obtained from memory__malloc().  NULL is a no-op.  Passing a
 * pointer not obtained from memory__malloc(), or one already freed, is
 * undefined behaviour, matching libc free(). */
void memory__free(void *ptr);

bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats);
