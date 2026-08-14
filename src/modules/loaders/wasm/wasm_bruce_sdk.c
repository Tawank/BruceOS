#include "wasm_bruce_sdk.h"
#include "wasm_bruce_abi.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm_export.h"

#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

_Static_assert(sizeof(uint32_t) == 4, "Bruce WASM SDK requires 32-bit uint32_t");
_Static_assert(WASM_BRUCE_MEMORY_STATS_SIZE == 11 * sizeof(uint32_t), "wasm32 memory stats ABI changed");

static bool s_registered;

static wasm_module_inst_t wasm_bruce_sdk__instance(wasm_exec_env_t exec_env) {
    if (exec_env == NULL) return NULL;
    return wasm_runtime_get_module_inst(exec_env);
}

static void *wasm_bruce_sdk__required_span(wasm_exec_env_t exec_env, uint32_t offset, uint32_t size) {
    wasm_module_inst_t instance = wasm_bruce_sdk__instance(exec_env);
    if (instance == NULL || !wasm_bruce_abi__required_span(offset, size)) return NULL;
    if (!wasm_runtime_validate_app_addr(instance, offset, size)) {
        wasm_runtime_clear_exception(instance);
        return NULL;
    }
    return wasm_runtime_addr_app_to_native(instance, offset);
}

static uint32_t *wasm_bruce_sdk__app_u32(wasm_exec_env_t exec_env, uint32_t offset) {
    return wasm_bruce_sdk__required_span(exec_env, offset, sizeof(uint32_t));
}

static uint64_t wasm_runtime__now(wasm_exec_env_t exec_env) {
    (void)exec_env;
    return runtime__now();
}

static int32_t wasm_runtime__sleep(wasm_exec_env_t exec_env, uint32_t milliseconds) {
    (void)exec_env;
    return runtime__sleep(milliseconds);
}

static int32_t wasm_runtime__delay(wasm_exec_env_t exec_env, uint32_t milliseconds) {
    (void)exec_env;
    return runtime__delay(milliseconds);
}

static int32_t wasm_runtime__gui_requested(wasm_exec_env_t exec_env) {
    (void)exec_env;
    return runtime__gui_requested() ? 1 : 0;
}

static uint32_t wasm_process__current_id(wasm_exec_env_t exec_env) {
    (void)exec_env;
    return process__current_id();
}

static int32_t wasm_process__current_signal(wasm_exec_env_t exec_env) {
    (void)exec_env;
    return process__current_signal();
}

#define WASM_NO_ARG_RESULT_WRAPPER(wrapper, function)                                                        \
    static int32_t wrapper(wasm_exec_env_t exec_env) {                                                       \
        (void)exec_env;                                                                                      \
        return function();                                                                                   \
    }

#define WASM_PROCESS_ID_RESULT_WRAPPER(wrapper, function)                                                    \
    static int32_t wrapper(wasm_exec_env_t exec_env, uint32_t id) {                                          \
        (void)exec_env;                                                                                      \
        return function((bruce_process_id_t)id);                                                             \
    }

WASM_NO_ARG_RESULT_WRAPPER(wasm_process__switch_next, process__switch_next)
WASM_NO_ARG_RESULT_WRAPPER(wasm_process__switch_previous, process__switch_previous)
WASM_NO_ARG_RESULT_WRAPPER(wasm_process__to_background, process__to_background)
WASM_NO_ARG_RESULT_WRAPPER(wasm_process__to_foreground, process__to_foreground)
WASM_PROCESS_ID_RESULT_WRAPPER(wasm_process__foreground, process__foreground)
WASM_PROCESS_ID_RESULT_WRAPPER(wasm_process__terminate, process__terminate)
WASM_PROCESS_ID_RESULT_WRAPPER(wasm_process__pause, process__pause)
WASM_PROCESS_ID_RESULT_WRAPPER(wasm_process__resume, process__resume)
WASM_PROCESS_ID_RESULT_WRAPPER(wasm_process__kill, process__kill)

static int32_t wasm_process__signal(wasm_exec_env_t exec_env, uint32_t id, int32_t signal) {
    (void)exec_env;
    return process__signal((bruce_process_id_t)id, (bruce_process_signal_t)signal);
}

static int32_t wasm_process__wait(wasm_exec_env_t exec_env, uint32_t id, uint32_t timeout_ms) {
    (void)exec_env;
    return process__wait((bruce_process_id_t)id, timeout_ms);
}

static int32_t wasm_permission__check(wasm_exec_env_t exec_env, int32_t permission) {
    (void)exec_env;
    return permission__check((bruce_permission_t)permission);
}

static int32_t wasm_stdio__read(
    wasm_exec_env_t exec_env, uint32_t buffer_offset, uint32_t capacity, uint32_t timeout_ms,
    uint32_t out_size_offset
) {
    if (capacity == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    void *buffer = wasm_bruce_sdk__required_span(exec_env, buffer_offset, capacity);
    uint32_t *out_size = wasm_bruce_sdk__app_u32(exec_env, out_size_offset);
    if (buffer == NULL || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    size_t size = 0;
    bruce_result_t result = stdio__read(buffer, capacity, timeout_ms, &size);
    if (size > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    wasm_bruce_abi__store_u32(out_size, (uint32_t)size);
    return result;
}

static int32_t wasm_stdio__read_line(
    wasm_exec_env_t exec_env, uint32_t buffer_offset, uint32_t buffer_size, int32_t mask_input
) {
    if (buffer_size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    char *buffer = wasm_bruce_sdk__required_span(exec_env, buffer_offset, buffer_size);
    if (buffer == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return stdio__read_line(buffer, buffer_size, mask_input != 0);
}

static int32_t wasm_stdio__write(wasm_exec_env_t exec_env, uint32_t data_offset, uint32_t size) {
    if (size == 0) return stdio__write(NULL, 0);
    const void *data = wasm_bruce_sdk__required_span(exec_env, data_offset, size);
    return data != NULL ? stdio__write(data, size) : BRUCE_ERR_INVALID_ARGUMENT;
}

static int32_t wasm_stdio__session_create(wasm_exec_env_t exec_env, uint32_t out_session_offset) {
    uint32_t *out_session = wasm_bruce_sdk__app_u32(exec_env, out_session_offset);
    if (out_session == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    bruce_result_t result = stdio__session_create(&session);
    wasm_bruce_abi__store_u32(out_session, session);
    return result;
}

static int32_t wasm_stdio__session_close(wasm_exec_env_t exec_env, uint32_t session) {
    (void)exec_env;
    return stdio__session_close((bruce_stdio_session_t)session);
}

static int32_t wasm_stdio__session_route_children(wasm_exec_env_t exec_env, uint32_t session) {
    (void)exec_env;
    return stdio__session_route_children((bruce_stdio_session_t)session);
}

static int32_t wasm_stdio__session_write_input(
    wasm_exec_env_t exec_env, uint32_t session, uint32_t data_offset, uint32_t size
) {
    if (size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    const void *data = wasm_bruce_sdk__required_span(exec_env, data_offset, size);
    if (data == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return stdio__session_write_input((bruce_stdio_session_t)session, data, size);
}

static int32_t wasm_stdio__session_read_output(
    wasm_exec_env_t exec_env, uint32_t session, uint32_t buffer_offset, uint32_t capacity,
    uint32_t out_size_offset
) {
    if (capacity == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    void *buffer = wasm_bruce_sdk__required_span(exec_env, buffer_offset, capacity);
    uint32_t *out_size = wasm_bruce_sdk__app_u32(exec_env, out_size_offset);
    if (buffer == NULL || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    size_t size = 0;
    bruce_result_t result =
        stdio__session_read_output((bruce_stdio_session_t)session, buffer, capacity, &size);
    if (size > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    wasm_bruce_abi__store_u32(out_size, (uint32_t)size);
    return result;
}

static int32_t wasm_memory__get_stats(wasm_exec_env_t exec_env, uint32_t stats_offset) {
    uint8_t *output = wasm_bruce_sdk__required_span(exec_env, stats_offset, WASM_BRUCE_MEMORY_STATS_SIZE);
    if (output == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_memory_stats_t stats;
    bruce_result_t result = memory__get_stats(&stats);
    if (result != BRUCE_OK) return result;
    const size_t values[] = {
        stats.internal_total,
        stats.internal_free,
        stats.internal_largest_block,
        stats.internal_minimum_free,
        stats.internal_free_blocks,
        stats.psram_total,
        stats.psram_free,
        stats.psram_largest_block,
        stats.swap_total,
        stats.swap_free,
        stats.swap_largest_block,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i] > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        wasm_bruce_abi__store_u32(output + i * sizeof(uint32_t), (uint32_t)values[i]);
    }
    return BRUCE_OK;
}

#define BRUCE_WASM_NATIVE(name, function, signature) {name, (void *)(function), signature, NULL}

/* WAMR sorts this array in place and retains it after registration. Pointer
 * parameters intentionally use i32 signatures so every accessed byte is
 * validated explicitly before conversion. */
static NativeSymbol s_native_symbols[] = {
    BRUCE_WASM_NATIVE("runtime__now", wasm_runtime__now, "()I"),
    BRUCE_WASM_NATIVE("runtime__sleep", wasm_runtime__sleep, "(i)i"),
    BRUCE_WASM_NATIVE("runtime__delay", wasm_runtime__delay, "(i)i"),
    BRUCE_WASM_NATIVE("runtime__gui_requested", wasm_runtime__gui_requested, "()i"),
    BRUCE_WASM_NATIVE("process__current_id", wasm_process__current_id, "()i"),
    BRUCE_WASM_NATIVE("process__current_signal", wasm_process__current_signal, "()i"),
    BRUCE_WASM_NATIVE("process__switch_next", wasm_process__switch_next, "()i"),
    BRUCE_WASM_NATIVE("process__switch_previous", wasm_process__switch_previous, "()i"),
    BRUCE_WASM_NATIVE("process__to_background", wasm_process__to_background, "()i"),
    BRUCE_WASM_NATIVE("process__to_foreground", wasm_process__to_foreground, "()i"),
    BRUCE_WASM_NATIVE("process__foreground", wasm_process__foreground, "(i)i"),
    BRUCE_WASM_NATIVE("process__signal", wasm_process__signal, "(ii)i"),
    BRUCE_WASM_NATIVE("process__terminate", wasm_process__terminate, "(i)i"),
    BRUCE_WASM_NATIVE("process__pause", wasm_process__pause, "(i)i"),
    BRUCE_WASM_NATIVE("process__resume", wasm_process__resume, "(i)i"),
    BRUCE_WASM_NATIVE("process__kill", wasm_process__kill, "(i)i"),
    BRUCE_WASM_NATIVE("process__wait", wasm_process__wait, "(ii)i"),
    BRUCE_WASM_NATIVE("permission__check", wasm_permission__check, "(i)i"),
    BRUCE_WASM_NATIVE("stdio__read", wasm_stdio__read, "(iiii)i"),
    BRUCE_WASM_NATIVE("stdio__read_line", wasm_stdio__read_line, "(iii)i"),
    BRUCE_WASM_NATIVE("stdio__write", wasm_stdio__write, "(ii)i"),
    BRUCE_WASM_NATIVE("stdio__session_create", wasm_stdio__session_create, "(i)i"),
    BRUCE_WASM_NATIVE("stdio__session_close", wasm_stdio__session_close, "(i)i"),
    BRUCE_WASM_NATIVE("stdio__session_route_children", wasm_stdio__session_route_children, "(i)i"),
    BRUCE_WASM_NATIVE("stdio__session_write_input", wasm_stdio__session_write_input, "(iii)i"),
    BRUCE_WASM_NATIVE("stdio__session_read_output", wasm_stdio__session_read_output, "(iiii)i"),
    BRUCE_WASM_NATIVE("memory__get_stats", wasm_memory__get_stats, "(i)i"),
};

bool wasm_bruce_sdk__register(void) {
    if (s_registered) return true;
    if (!wasm_runtime_register_natives(
            "bruce_sdk", s_native_symbols, (uint32_t)(sizeof(s_native_symbols) / sizeof(s_native_symbols[0]))
        )) {
        return false;
    }
    s_registered = true;
    return true;
}
