#include "wasm_bruce_host_adapter.h"
#include "wasm_bruce_abi.h"

#include <limits.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm_export.h"

#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"

_Static_assert(sizeof(uint32_t) == 4, "Bruce WASM SDK requires 32-bit uint32_t");
_Static_assert(WASM_BRUCE_MEMORY_STATS_SIZE == 11 * sizeof(uint32_t), "wasm32 memory stats ABI changed");

static bool s_registered;

static wasm_module_inst_t wasm_bruce_host_adapter__instance(wasm_exec_env_t exec_env) {
    if (exec_env == NULL) return NULL;
    return wasm_runtime_get_module_inst(exec_env);
}

static void *
wasm_bruce_host_adapter__required_span(wasm_exec_env_t exec_env, uint32_t offset, uint32_t size) {
    wasm_module_inst_t instance = wasm_bruce_host_adapter__instance(exec_env);
    if (instance == NULL || !wasm_bruce_abi__required_span(offset, size)) return NULL;
    if (!wasm_runtime_validate_app_addr(instance, offset, size)) {
        wasm_runtime_clear_exception(instance);
        return NULL;
    }
    return wasm_runtime_addr_app_to_native(instance, offset);
}

static uint32_t *wasm_bruce_host_adapter__app_u32(wasm_exec_env_t exec_env, uint32_t offset) {
    return wasm_bruce_host_adapter__required_span(exec_env, offset, sizeof(uint32_t));
}

static const char *wasm_bruce_host_adapter__string(wasm_exec_env_t exec_env, uint32_t offset, bool optional) {
    if (offset == 0) return optional ? NULL : (const char *)UINTPTR_MAX;
    wasm_module_inst_t instance = wasm_bruce_host_adapter__instance(exec_env);
    if (instance == NULL || !wasm_runtime_validate_app_str_addr(instance, offset)) {
        if (instance != NULL) wasm_runtime_clear_exception(instance);
        return (const char *)UINTPTR_MAX;
    }
    return wasm_runtime_addr_app_to_native(instance, offset);
}

static bool wasm_bruce_host_adapter__valid_string(const char *value) {
    return value != (const char *)UINTPTR_MAX;
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
    void *buffer = wasm_bruce_host_adapter__required_span(exec_env, buffer_offset, capacity);
    uint32_t *out_size = wasm_bruce_host_adapter__app_u32(exec_env, out_size_offset);
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
    char *buffer = wasm_bruce_host_adapter__required_span(exec_env, buffer_offset, buffer_size);
    if (buffer == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return stdio__read_line(buffer, buffer_size, mask_input != 0);
}

static int32_t wasm_stdio__write(wasm_exec_env_t exec_env, uint32_t data_offset, uint32_t size) {
    if (size == 0) return stdio__write(NULL, 0);
    const void *data = wasm_bruce_host_adapter__required_span(exec_env, data_offset, size);
    return data != NULL ? stdio__write(data, size) : BRUCE_ERR_INVALID_ARGUMENT;
}

static int32_t wasm_stdio__session_create(wasm_exec_env_t exec_env, uint32_t out_session_offset) {
    uint32_t *out_session = wasm_bruce_host_adapter__app_u32(exec_env, out_session_offset);
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
    const void *data = wasm_bruce_host_adapter__required_span(exec_env, data_offset, size);
    if (data == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return stdio__session_write_input((bruce_stdio_session_t)session, data, size);
}

static int32_t wasm_stdio__session_read_output(
    wasm_exec_env_t exec_env, uint32_t session, uint32_t buffer_offset, uint32_t capacity,
    uint32_t out_size_offset
) {
    if (capacity == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    void *buffer = wasm_bruce_host_adapter__required_span(exec_env, buffer_offset, capacity);
    uint32_t *out_size = wasm_bruce_host_adapter__app_u32(exec_env, out_size_offset);
    if (buffer == NULL || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    size_t size = 0;
    bruce_result_t result =
        stdio__session_read_output((bruce_stdio_session_t)session, buffer, capacity, &size);
    if (size > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    wasm_bruce_abi__store_u32(out_size, (uint32_t)size);
    return result;
}

static int32_t wasm_tty__isatty(wasm_exec_env_t exec_env) {
    (void)exec_env;
    return tty__isatty() ? 1 : 0;
}

static int32_t wasm_tty__get_size(wasm_exec_env_t exec_env, uint32_t out_size_offset) {
    uint8_t *output =
        wasm_bruce_host_adapter__required_span(exec_env, out_size_offset, WASM_BRUCE_TTY_SIZE_SIZE);
    if (output == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_tty_size_t size;
    bruce_result_t result = tty__get_size(&size);
    if (result != BRUCE_OK) return result;
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_TTY_SIZE_COLUMNS_OFFSET, size.columns);
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_TTY_SIZE_ROWS_OFFSET, size.rows);
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_TTY_SIZE_GENERATION_OFFSET, size.generation);
    return BRUCE_OK;
}

static int32_t
wasm_tty__set_size(wasm_exec_env_t exec_env, uint32_t session, uint32_t columns, uint32_t rows) {
    (void)exec_env;
    if (columns > UINT16_MAX || rows > UINT16_MAX) return BRUCE_ERR_INVALID_ARGUMENT;
    return tty__set_size((bruce_stdio_session_t)session, (uint16_t)columns, (uint16_t)rows);
}

static int32_t wasm_tty__get_mode(wasm_exec_env_t exec_env) {
    (void)exec_env;
    return (int32_t)tty__get_mode();
}

static int32_t wasm_tty__set_mode(wasm_exec_env_t exec_env, uint32_t mode) {
    (void)exec_env;
    return tty__set_mode((bruce_tty_mode_t)mode);
}

static int32_t wasm_memory__get_stats(wasm_exec_env_t exec_env, uint32_t stats_offset) {
    uint8_t *output =
        wasm_bruce_host_adapter__required_span(exec_env, stats_offset, WASM_BRUCE_MEMORY_STATS_SIZE);
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

static uint32_t wasm_memory__malloc(wasm_exec_env_t exec_env, uint32_t size) {
    wasm_module_inst_t instance = wasm_bruce_host_adapter__instance(exec_env);
    if (instance == NULL || size == 0) return 0;
    return wasm_runtime_module_malloc(instance, size, NULL);
}

static void wasm_memory__free(wasm_exec_env_t exec_env, uint32_t pointer_offset) {
    wasm_module_inst_t instance = wasm_bruce_host_adapter__instance(exec_env);
    if (instance != NULL && pointer_offset != 0) wasm_runtime_module_free(instance, pointer_offset);
}

static uint32_t wasm_config__get_time_clock24hr(wasm_exec_env_t exec_env) {
    (void)exec_env;
    return config__get_time_clock24hr() ? 1u : 0u;
}

#define WASM_SCALAR_U32_WRAPPER(wrapper, function)                                                            \
    static uint32_t wrapper(wasm_exec_env_t exec_env) {                                                       \
        (void)exec_env;                                                                                       \
        return (uint32_t)function();                                                                          \
    }

WASM_SCALAR_U32_WRAPPER(wasm_config__get_theme_primary, config__get_theme_primary)
WASM_SCALAR_U32_WRAPPER(wasm_config__get_theme_secondary, config__get_theme_secondary)
WASM_SCALAR_U32_WRAPPER(wasm_config__get_theme_background, config__get_theme_background)
WASM_SCALAR_U32_WRAPPER(wasm_display__width, display__width)
WASM_SCALAR_U32_WRAPPER(wasm_display__height, display__height)
WASM_NO_ARG_RESULT_WRAPPER(wasm_display__begin_frame, display__begin_frame)
WASM_NO_ARG_RESULT_WRAPPER(wasm_display__present, display__present)
WASM_NO_ARG_RESULT_WRAPPER(wasm_input__flush, input__flush)

static int32_t wasm_display__fill_screen(wasm_exec_env_t exec_env, uint32_t color) {
    (void)exec_env;
    return display__fill_screen((bruce_display_color_t)color);
}

static int32_t wasm_display__draw_rect(
    wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color
) {
    (void)exec_env;
    return display__draw_rect(
        (int16_t)x, (int16_t)y, (int16_t)width, (int16_t)height, (bruce_display_color_t)color
    );
}

static int32_t wasm_display__set_text_bg_color(wasm_exec_env_t exec_env, uint32_t color) {
    (void)exec_env;
    return display__set_text_bg_color(color);
}

static int32_t wasm_display__set_text_color(wasm_exec_env_t exec_env, uint32_t color) {
    (void)exec_env;
    return display__set_text_color((bruce_display_color_t)color);
}

static int32_t wasm_display__set_text_size(wasm_exec_env_t exec_env, uint32_t size) {
    (void)exec_env;
    return display__set_text_size((uint8_t)size);
}

static int32_t
wasm_display__draw_centre_string(wasm_exec_env_t exec_env, uint32_t text_offset, int32_t x, int32_t y) {
    const char *text = wasm_bruce_host_adapter__string(exec_env, text_offset, false);
    if (!wasm_bruce_host_adapter__valid_string(text)) return BRUCE_ERR_INVALID_ARGUMENT;
    return display__draw_centre_string(text, (int16_t)x, (int16_t)y);
}

static int32_t wasm_clock__get_local(wasm_exec_env_t exec_env, uint32_t output_offset) {
    uint8_t *output = wasm_bruce_host_adapter__required_span(
        exec_env, output_offset, WASM_BRUCE_CLOCK_DATETIME_SIZE
    );
    if (output == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_clock_datetime_t value;
    bruce_result_t result = clock__get_local(&value);
    if (result != BRUCE_OK) return result;
    memset(output, 0, WASM_BRUCE_CLOCK_DATETIME_SIZE);
    wasm_bruce_abi__store_u16(output + WASM_BRUCE_CLOCK_DATETIME_YEAR_OFFSET, value.year);
    output[WASM_BRUCE_CLOCK_DATETIME_MONTH_OFFSET] = value.month;
    output[WASM_BRUCE_CLOCK_DATETIME_DAY_OFFSET] = value.day;
    output[WASM_BRUCE_CLOCK_DATETIME_HOUR_OFFSET] = value.hour;
    output[WASM_BRUCE_CLOCK_DATETIME_MINUTE_OFFSET] = value.minute;
    output[WASM_BRUCE_CLOCK_DATETIME_SECOND_OFFSET] = value.second;
    return BRUCE_OK;
}

static int32_t
wasm_process__snapshot(wasm_exec_env_t exec_env, uint32_t process_id, uint32_t output_offset) {
    uint8_t *output = wasm_bruce_host_adapter__required_span(
        exec_env, output_offset, WASM_BRUCE_PROCESS_SNAPSHOT_SIZE
    );
    if (output == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_snapshot_t snapshot;
    bruce_result_t result = process__snapshot((bruce_process_id_t)process_id, &snapshot);
    if (result != BRUCE_OK) return result;
    if (snapshot.memory_bytes > UINT32_MAX || snapshot.swap_bytes > UINT32_MAX ||
        snapshot.resource_count > UINT32_MAX) {
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    memset(output, 0, WASM_BRUCE_PROCESS_SNAPSHOT_SIZE);
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_PROCESS_SNAPSHOT_ID_OFFSET, snapshot.id);
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_PROCESS_SNAPSHOT_STATE_OFFSET, (uint32_t)snapshot.state);
    memcpy(output + WASM_BRUCE_PROCESS_SNAPSHOT_NAME_OFFSET, snapshot.name, WASM_BRUCE_PROCESS_SNAPSHOT_NAME_SIZE);
    output[WASM_BRUCE_PROCESS_SNAPSHOT_NAME_OFFSET + WASM_BRUCE_PROCESS_SNAPSHOT_NAME_SIZE - 1] = '\0';
    wasm_bruce_abi__store_u32(
        output + WASM_BRUCE_PROCESS_SNAPSHOT_STACK_HIGH_WATER_OFFSET, snapshot.stack_high_water_bytes
    );
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_PROCESS_SNAPSHOT_STACK_TOTAL_OFFSET, snapshot.stack_total_bytes);
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_PROCESS_SNAPSHOT_CPU_PERCENT_OFFSET, snapshot.cpu_percent);
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_PROCESS_SNAPSHOT_MEMORY_BYTES_OFFSET, (uint32_t)snapshot.memory_bytes);
    wasm_bruce_abi__store_u32(output + WASM_BRUCE_PROCESS_SNAPSHOT_SWAP_BYTES_OFFSET, (uint32_t)snapshot.swap_bytes);
    wasm_bruce_abi__store_u32(
        output + WASM_BRUCE_PROCESS_SNAPSHOT_RESOURCE_COUNT_OFFSET, (uint32_t)snapshot.resource_count
    );
    output[WASM_BRUCE_PROCESS_SNAPSHOT_BUILT_IN_OFFSET] = snapshot.built_in ? 1 : 0;
    output[WASM_BRUCE_PROCESS_SNAPSHOT_GUI_REQUESTED_OFFSET] = snapshot.gui_requested ? 1 : 0;
    output[WASM_BRUCE_PROCESS_SNAPSHOT_PRESENTABLE_OFFSET] = snapshot.presentable ? 1 : 0;
    return BRUCE_OK;
}

static int32_t wasm_input__wait(wasm_exec_env_t exec_env, uint32_t timeout_ms, uint32_t output_offset) {
    uint32_t *output = wasm_bruce_host_adapter__app_u32(exec_env, output_offset);
    if (output == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    int32_t code;
    bruce_result_t result = input__wait(timeout_ms, &code);
    if (result == BRUCE_OK) wasm_bruce_abi__store_u32(output, (uint32_t)code);
    return result;
}

static int32_t
wasm_dialog__message(wasm_exec_env_t exec_env, int32_t kind, uint32_t title_offset, uint32_t message_offset) {
    const char *title = wasm_bruce_host_adapter__string(exec_env, title_offset, true);
    const char *message = wasm_bruce_host_adapter__string(exec_env, message_offset, true);
    if (!wasm_bruce_host_adapter__valid_string(title) || !wasm_bruce_host_adapter__valid_string(message)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return dialog__message((bruce_dialog_kind_t)kind, title, message);
}

static int32_t wasm_dialog__choice(
    wasm_exec_env_t exec_env, uint32_t title_offset, uint32_t message_offset, uint32_t choices_offset,
    uint32_t choice_count, uint32_t selected_offset
) {
    if (choice_count == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    if (choice_count > WASM_BRUCE_MAX_DIALOG_CHOICES) return BRUCE_ERR_RESOURCE_LIMIT;
    uint32_t choices_size;
    if (!wasm_bruce_abi__array_span(
            choices_offset, choice_count, WASM_BRUCE_DIALOG_CHOICE_SIZE, &choices_size
        )) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    const uint8_t *guest_choices = wasm_bruce_host_adapter__required_span(exec_env, choices_offset, choices_size);
    uint32_t *guest_selected = wasm_bruce_host_adapter__app_u32(exec_env, selected_offset);
    const char *title = wasm_bruce_host_adapter__string(exec_env, title_offset, true);
    const char *message = wasm_bruce_host_adapter__string(exec_env, message_offset, true);
    if (guest_choices == NULL || guest_selected == NULL || !wasm_bruce_host_adapter__valid_string(title) ||
        !wasm_bruce_host_adapter__valid_string(message)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_dialog_choice_t choices[WASM_BRUCE_MAX_DIALOG_CHOICES];
    size_t text_bytes = 0;
    for (uint32_t i = 0; i < choice_count; ++i) {
        const uint8_t *record = guest_choices + i * WASM_BRUCE_DIALOG_CHOICE_SIZE;
        const uint32_t offsets[] = {
            wasm_bruce_abi__load_u32(record + WASM_BRUCE_DIALOG_CHOICE_LABEL_OFFSET),
            wasm_bruce_abi__load_u32(record + WASM_BRUCE_DIALOG_CHOICE_VALUE_OFFSET),
            wasm_bruce_abi__load_u32(record + WASM_BRUCE_DIALOG_CHOICE_ICON_NAME_OFFSET),
            wasm_bruce_abi__load_u32(record + WASM_BRUCE_DIALOG_CHOICE_RIGHT_TEXT_OFFSET),
        };
        const char **fields[] = {&choices[i].label, &choices[i].value, &choices[i].icon_name, &choices[i].right_text};
        for (size_t field = 0; field < 4; ++field) {
            *fields[field] = wasm_bruce_host_adapter__string(exec_env, offsets[field], field >= 2);
            if (!wasm_bruce_host_adapter__valid_string(*fields[field])) return BRUCE_ERR_INVALID_ARGUMENT;
            if (*fields[field] != NULL) {
                size_t length = strlen(*fields[field]) + 1;
                if (length > WASM_BRUCE_MAX_DIALOG_TEXT_BYTES - text_bytes) return BRUCE_ERR_RESOURCE_LIMIT;
                text_bytes += length;
            }
        }
    }
    size_t selected = wasm_bruce_abi__load_u32(guest_selected);
    bruce_result_t result = dialog__choice(title, message, choices, choice_count, &selected);
    if (result == BRUCE_OK) {
        if (selected > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
        wasm_bruce_abi__store_u32(guest_selected, (uint32_t)selected);
    }
    return result;
}

static int32_t wasm_dialog__number_input(
    wasm_exec_env_t exec_env, uint32_t title_offset, uint32_t prompt_offset, uint32_t initial_offset,
    uint32_t buffer_offset, uint32_t buffer_size
) {
    if (buffer_size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    const char *title = wasm_bruce_host_adapter__string(exec_env, title_offset, true);
    const char *prompt = wasm_bruce_host_adapter__string(exec_env, prompt_offset, true);
    const char *initial = wasm_bruce_host_adapter__string(exec_env, initial_offset, true);
    char *buffer = wasm_bruce_host_adapter__required_span(exec_env, buffer_offset, buffer_size);
    if (!wasm_bruce_host_adapter__valid_string(title) || !wasm_bruce_host_adapter__valid_string(prompt) ||
        !wasm_bruce_host_adapter__valid_string(initial) || buffer == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return dialog__number_input(title, prompt, initial, buffer, buffer_size);
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
    BRUCE_WASM_NATIVE("tty__isatty", wasm_tty__isatty, "()i"),
    BRUCE_WASM_NATIVE("tty__get_size", wasm_tty__get_size, "(i)i"),
    BRUCE_WASM_NATIVE("tty__set_size", wasm_tty__set_size, "(iii)i"),
    BRUCE_WASM_NATIVE("tty__get_mode", wasm_tty__get_mode, "()i"),
    BRUCE_WASM_NATIVE("tty__set_mode", wasm_tty__set_mode, "(i)i"),
    BRUCE_WASM_NATIVE("memory__get_stats", wasm_memory__get_stats, "(i)i"),
    BRUCE_WASM_NATIVE("memory__malloc", wasm_memory__malloc, "(i)i"),
    BRUCE_WASM_NATIVE("memory__free", wasm_memory__free, "(i)"),
    BRUCE_WASM_NATIVE("config__get_time_clock24hr", wasm_config__get_time_clock24hr, "()i"),
    BRUCE_WASM_NATIVE("config__get_theme_primary", wasm_config__get_theme_primary, "()i"),
    BRUCE_WASM_NATIVE("config__get_theme_secondary", wasm_config__get_theme_secondary, "()i"),
    BRUCE_WASM_NATIVE("config__get_theme_background", wasm_config__get_theme_background, "()i"),
    BRUCE_WASM_NATIVE("display__width", wasm_display__width, "()i"),
    BRUCE_WASM_NATIVE("display__height", wasm_display__height, "()i"),
    BRUCE_WASM_NATIVE("display__begin_frame", wasm_display__begin_frame, "()i"),
    BRUCE_WASM_NATIVE("display__fill_screen", wasm_display__fill_screen, "(i)i"),
    BRUCE_WASM_NATIVE("display__draw_rect", wasm_display__draw_rect, "(iiiii)i"),
    BRUCE_WASM_NATIVE("display__set_text_bg_color", wasm_display__set_text_bg_color, "(i)i"),
    BRUCE_WASM_NATIVE("display__set_text_color", wasm_display__set_text_color, "(i)i"),
    BRUCE_WASM_NATIVE("display__set_text_size", wasm_display__set_text_size, "(i)i"),
    BRUCE_WASM_NATIVE("display__draw_centre_string", wasm_display__draw_centre_string, "(iii)i"),
    BRUCE_WASM_NATIVE("display__present", wasm_display__present, "()i"),
    BRUCE_WASM_NATIVE("clock__get_local", wasm_clock__get_local, "(i)i"),
    BRUCE_WASM_NATIVE("process__snapshot", wasm_process__snapshot, "(ii)i"),
    BRUCE_WASM_NATIVE("input__flush", wasm_input__flush, "()i"),
    BRUCE_WASM_NATIVE("input__wait", wasm_input__wait, "(ii)i"),
    BRUCE_WASM_NATIVE("dialog__message", wasm_dialog__message, "(iii)i"),
    BRUCE_WASM_NATIVE("dialog__choice", wasm_dialog__choice, "(iiiii)i"),
    BRUCE_WASM_NATIVE("dialog__number_input", wasm_dialog__number_input, "(iiiii)i"),
};

bool wasm_bruce_host_adapter__register(void) {
    if (s_registered) return true;
    if (!wasm_runtime_register_natives(
            "bruce_sdk", s_native_symbols, (uint32_t)(sizeof(s_native_symbols) / sizeof(s_native_symbols[0]))
        )) {
        return false;
    }
    s_registered = true;
    return true;
}
