#include "wasm_bruce_sdk_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/loaders/wasm/wasm_bruce_abi.h"

bool selftest__run_wasm_bruce_abi_case(void) {
    uint8_t bytes[8];
    memset(bytes, 0, sizeof(bytes));
    wasm_bruce_abi__store_u16(bytes + 1, UINT16_C(0x1234));
    if (wasm_bruce_abi__load_u16(bytes + 1) != UINT16_C(0x1234)) return false;

    wasm_bruce_abi__store_u32(bytes + 1, UINT32_C(0x89abcdef));
    if (wasm_bruce_abi__load_u32(bytes + 1) != UINT32_C(0x89abcdef)) return false;

    wasm_bruce_abi__store_u64(bytes, UINT64_C(0x0123456789abcdef));
    if (wasm_bruce_abi__load_u64(bytes) != UINT64_C(0x0123456789abcdef)) return false;

    uint32_t span_size = 0;
    if (!wasm_bruce_abi__required_span(1, 1) || wasm_bruce_abi__required_span(0, 1) ||
        wasm_bruce_abi__required_span(1, 0) || wasm_bruce_abi__required_span(UINT32_MAX, 1) ||
        wasm_bruce_abi__required_span(UINT32_MAX - 1, 2) ||
        !wasm_bruce_abi__optional_span(0, 1) || !wasm_bruce_abi__optional_span(16, 4) ||
        wasm_bruce_abi__optional_span(UINT32_MAX, 1) ||
        !wasm_bruce_abi__array_span(16, 3, 4, &span_size) || span_size != 12 ||
        wasm_bruce_abi__array_span(16, UINT32_MAX, 2, &span_size)) {
        printf("[selftest] wasm ABI: span validation failed\n");
        return false;
    }

    if (WASM_BRUCE_CLOCK_DATETIME_SIZE != 8 || WASM_BRUCE_PROCESS_SNAPSHOT_SIZE != 100 ||
        WASM_BRUCE_DIALOG_CHOICE_SIZE != 16) {
        printf("[selftest] wasm ABI: layout size changed\n");
        return false;
    }
    return true;
}
