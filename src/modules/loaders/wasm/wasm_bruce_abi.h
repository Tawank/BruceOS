#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private wasm32 layouts. Host wrappers must encode fields individually rather
 * than copying native Core structures into guest memory. */
enum {
    WASM_BRUCE_MAX_DIALOG_CHOICES = 16,
    WASM_BRUCE_MAX_DIALOG_TEXT_BYTES = 2048,
    WASM_BRUCE_MEMORY_STATS_SIZE = 44,

    WASM_BRUCE_CLOCK_DATETIME_YEAR_OFFSET = 0,
    WASM_BRUCE_CLOCK_DATETIME_MONTH_OFFSET = 2,
    WASM_BRUCE_CLOCK_DATETIME_DAY_OFFSET = 3,
    WASM_BRUCE_CLOCK_DATETIME_HOUR_OFFSET = 4,
    WASM_BRUCE_CLOCK_DATETIME_MINUTE_OFFSET = 5,
    WASM_BRUCE_CLOCK_DATETIME_SECOND_OFFSET = 6,
    WASM_BRUCE_CLOCK_DATETIME_SIZE = 8,

    WASM_BRUCE_PROCESS_SNAPSHOT_ID_OFFSET = 0,
    WASM_BRUCE_PROCESS_SNAPSHOT_STATE_OFFSET = 4,
    WASM_BRUCE_PROCESS_SNAPSHOT_NAME_OFFSET = 8,
    WASM_BRUCE_PROCESS_SNAPSHOT_NAME_SIZE = 64,
    WASM_BRUCE_PROCESS_SNAPSHOT_STACK_HIGH_WATER_OFFSET = 72,
    WASM_BRUCE_PROCESS_SNAPSHOT_STACK_TOTAL_OFFSET = 76,
    WASM_BRUCE_PROCESS_SNAPSHOT_CPU_PERCENT_OFFSET = 80,
    WASM_BRUCE_PROCESS_SNAPSHOT_MEMORY_BYTES_OFFSET = 84,
    WASM_BRUCE_PROCESS_SNAPSHOT_SWAP_BYTES_OFFSET = 88,
    WASM_BRUCE_PROCESS_SNAPSHOT_RESOURCE_COUNT_OFFSET = 92,
    WASM_BRUCE_PROCESS_SNAPSHOT_BUILT_IN_OFFSET = 96,
    WASM_BRUCE_PROCESS_SNAPSHOT_GUI_REQUESTED_OFFSET = 97,
    WASM_BRUCE_PROCESS_SNAPSHOT_PRESENTABLE_OFFSET = 98,
    WASM_BRUCE_PROCESS_SNAPSHOT_SIZE = 100,

    WASM_BRUCE_TTY_SIZE_COLUMNS_OFFSET = 0,
    WASM_BRUCE_TTY_SIZE_ROWS_OFFSET = 4,
    WASM_BRUCE_TTY_SIZE_GENERATION_OFFSET = 8,
    WASM_BRUCE_TTY_SIZE_SIZE = 12,

    WASM_BRUCE_DIALOG_CHOICE_LABEL_OFFSET = 0,
    WASM_BRUCE_DIALOG_CHOICE_VALUE_OFFSET = 4,
    WASM_BRUCE_DIALOG_CHOICE_ICON_NAME_OFFSET = 8,
    WASM_BRUCE_DIALOG_CHOICE_RIGHT_TEXT_OFFSET = 12,
    WASM_BRUCE_DIALOG_CHOICE_SIZE = 16,
};

static inline bool wasm_bruce_abi__required_span(uint32_t offset, uint32_t size) {
    return offset != 0 && size != 0 && size <= UINT32_MAX - offset;
}

static inline bool wasm_bruce_abi__optional_span(uint32_t offset, uint32_t size) {
    return offset == 0 || wasm_bruce_abi__required_span(offset, size);
}

static inline bool
wasm_bruce_abi__array_span(uint32_t offset, uint32_t count, uint32_t element_size, uint32_t *out_size) {
    if (out_size == NULL || count == 0 || element_size == 0 || count > UINT32_MAX / element_size) {
        return false;
    }
    uint32_t size = count * element_size;
    if (!wasm_bruce_abi__required_span(offset, size)) return false;
    *out_size = size;
    return true;
}

static inline uint16_t wasm_bruce_abi__load_u16(const void *source) {
    const uint8_t *bytes = source;
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static inline uint32_t wasm_bruce_abi__load_u32(const void *source) {
    const uint8_t *bytes = source;
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static inline uint64_t wasm_bruce_abi__load_u64(const void *source) {
    return (uint64_t)wasm_bruce_abi__load_u32(source) |
           ((uint64_t)wasm_bruce_abi__load_u32((const uint8_t *)source + 4) << 32);
}

static inline void wasm_bruce_abi__store_u16(void *destination, uint16_t value) {
    uint8_t *bytes = destination;
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static inline void wasm_bruce_abi__store_u32(void *destination, uint32_t value) {
    uint8_t *bytes = destination;
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static inline void wasm_bruce_abi__store_u64(void *destination, uint64_t value) {
    wasm_bruce_abi__store_u32(destination, (uint32_t)value);
    wasm_bruce_abi__store_u32((uint8_t *)destination + 4, (uint32_t)(value >> 32));
}
