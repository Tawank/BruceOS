#pragma once

#include <stddef.h>

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
