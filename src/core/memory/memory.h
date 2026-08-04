#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/memory.h"

/* Core-only variants used by the ELF loader's executable mappings. */
bruce_result_t memory_external__alloc(
    size_t size, bool executable, bruce_memory_object_t *out_object
);
bruce_result_t memory_external__adopt(bruce_memory_object_t *object);
bruce_result_t memory_external__instruction_map(
    const bruce_memory_object_t *object, const void **out_instruction
);
bruce_result_t memory_external__release(bruce_memory_object_t *object);
void memory_external__get_swap_stats(size_t *out_total, size_t *out_free, size_t *out_largest);

/*
 * Core-owned external-memory objects: PSRAM/swap allocations with no
 * process owner. Unlike memory__external_*(), these are never released by
 * process exit or process_registry cleanup - they live until an explicit
 * memory_external_core__free() call, matching the lifetime of Core
 * singleton state (config, status icons, ...) that must survive whichever
 * process last touched it. Only Core code may call these; the object
 * handles are not exposed across the core_sdk boundary.
 */
bruce_result_t memory_external_core__alloc(size_t size, bruce_memory_object_t *out_object);
bruce_result_t memory_external_core__write(
    const bruce_memory_object_t *object, size_t offset, const void *data, size_t size
);
bruce_result_t memory_external_core__map(const bruce_memory_object_t *object, const void **out_data);
bruce_result_t memory_external_core__free(bruce_memory_object_t *object);
