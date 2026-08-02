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
