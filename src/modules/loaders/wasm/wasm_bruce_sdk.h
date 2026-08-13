#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Register the Bruce SDK native import module after WAMR initialization and
 * before loading any module which imports from "bruce_sdk". Safe to call more
 * than once for the lifetime of one WAMR runtime. */
bool wasm_bruce_sdk__register(void);

/* wasm32 serialization written by memory__get_stats. All fields are unsigned
 * 32-bit little-endian integers, in this order. */
typedef struct {
    uint32_t internal_total;
    uint32_t internal_free;
    uint32_t internal_largest_block;
    uint32_t internal_minimum_free;
    uint32_t internal_free_blocks;
    uint32_t psram_total;
    uint32_t psram_free;
    uint32_t psram_largest_block;
    uint32_t swap_total;
    uint32_t swap_free;
    uint32_t swap_largest_block;
} wasm_bruce_memory_stats32_t;
