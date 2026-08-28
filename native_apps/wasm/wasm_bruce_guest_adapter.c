/* This file is compiled into WASM applications, never into the firmware. */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"
#include "core_sdk/audio.h"
#include "core_sdk/stdio.h"

#define BRUCE_WASM_IMPORT(name) __attribute__((import_module("bruce_sdk"), import_name(name)))

#ifndef BRUCE_WASM_ENTRY
#error "BRUCE_WASM_ENTRY must name the application's C entry point"
#endif

extern int BRUCE_WASM_ENTRY(int argc, char **argv);

__attribute__((export_name("main"))) int wasm_bruce_guest_adapter__main(int argc, char **argv) {
    return BRUCE_WASM_ENTRY(argc, argv);
}

_Static_assert(sizeof(size_t) == sizeof(uint32_t), "Bruce WASM guest requires wasm32 size_t");
_Static_assert(sizeof(bruce_permission_t) == sizeof(int32_t), "wasm32 permission enum ABI changed");
_Static_assert(sizeof(bruce_process_signal_t) == sizeof(int32_t), "wasm32 process signal enum ABI changed");
_Static_assert(sizeof(bruce_memory_stats_t) == 44, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, internal_total) == 0, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, internal_free) == 4, "wasm32 memory stats ABI changed");
_Static_assert(
    offsetof(bruce_memory_stats_t, internal_largest_block) == 8, "wasm32 memory stats ABI changed"
);
_Static_assert(
    offsetof(bruce_memory_stats_t, internal_minimum_free) == 12, "wasm32 memory stats ABI changed"
);
_Static_assert(offsetof(bruce_memory_stats_t, internal_free_blocks) == 16, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, psram_total) == 20, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, psram_free) == 24, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, psram_largest_block) == 28, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, swap_total) == 32, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, swap_free) == 36, "wasm32 memory stats ABI changed");
_Static_assert(offsetof(bruce_memory_stats_t, swap_largest_block) == 40, "wasm32 memory stats ABI changed");
_Static_assert(sizeof(bruce_clock_datetime_t) == 8, "wasm32 clock datetime ABI changed");
_Static_assert(offsetof(bruce_clock_datetime_t, year) == 0, "wasm32 clock datetime ABI changed");
_Static_assert(offsetof(bruce_clock_datetime_t, month) == 2, "wasm32 clock datetime ABI changed");
_Static_assert(offsetof(bruce_clock_datetime_t, day) == 3, "wasm32 clock datetime ABI changed");
_Static_assert(offsetof(bruce_clock_datetime_t, hour) == 4, "wasm32 clock datetime ABI changed");
_Static_assert(offsetof(bruce_clock_datetime_t, minute) == 5, "wasm32 clock datetime ABI changed");
_Static_assert(offsetof(bruce_clock_datetime_t, second) == 6, "wasm32 clock datetime ABI changed");
_Static_assert(sizeof(bruce_process_snapshot_t) == 100, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, id) == 0, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, state) == 4, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, name) == 8, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, stack_high_water_bytes) == 72, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, stack_total_bytes) == 76, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, cpu_percent) == 80, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, memory_bytes) == 84, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, swap_bytes) == 88, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, resource_count) == 92, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, built_in) == 96, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, gui_requested) == 97, "wasm32 process snapshot ABI changed");
_Static_assert(offsetof(bruce_process_snapshot_t, presentable) == 98, "wasm32 process snapshot ABI changed");
_Static_assert(sizeof(bruce_dialog_choice_t) == 16, "wasm32 dialog choice ABI changed");
_Static_assert(offsetof(bruce_dialog_choice_t, label) == 0, "wasm32 dialog choice ABI changed");
_Static_assert(offsetof(bruce_dialog_choice_t, value) == 4, "wasm32 dialog choice ABI changed");
_Static_assert(offsetof(bruce_dialog_choice_t, icon_name) == 8, "wasm32 dialog choice ABI changed");
_Static_assert(offsetof(bruce_dialog_choice_t, right_text) == 12, "wasm32 dialog choice ABI changed");

static uint32_t wasm_bruce_guest_adapter__offset(const volatile void *pointer) { return (uint32_t)(uintptr_t)pointer; }

BRUCE_WASM_IMPORT("runtime__now") extern uint64_t wasm_import__runtime_now(void);
BRUCE_WASM_IMPORT("runtime__sleep") extern int32_t wasm_import__runtime_sleep(uint32_t milliseconds);
BRUCE_WASM_IMPORT("runtime__delay") extern int32_t wasm_import__runtime_delay(uint32_t milliseconds);
BRUCE_WASM_IMPORT("runtime__gui_requested") extern int32_t wasm_import__runtime_gui_requested(void);

uint64_t runtime__now(void) { return wasm_import__runtime_now(); }

bruce_result_t runtime__sleep(uint32_t milliseconds) {
    return (bruce_result_t)wasm_import__runtime_sleep(milliseconds);
}

bruce_result_t runtime__delay(uint32_t milliseconds) {
    return (bruce_result_t)wasm_import__runtime_delay(milliseconds);
}

bool runtime__gui_requested(void) { return wasm_import__runtime_gui_requested() != 0; }

BRUCE_WASM_IMPORT("process__current_id") extern uint32_t wasm_import__process_current_id(void);
BRUCE_WASM_IMPORT("process__current_signal") extern int32_t wasm_import__process_current_signal(void);
BRUCE_WASM_IMPORT("process__switch_next") extern int32_t wasm_import__process_switch_next(void);
BRUCE_WASM_IMPORT("process__switch_previous") extern int32_t wasm_import__process_switch_previous(void);
BRUCE_WASM_IMPORT("process__to_background") extern int32_t wasm_import__process_to_background(void);
BRUCE_WASM_IMPORT("process__to_foreground") extern int32_t wasm_import__process_to_foreground(void);
BRUCE_WASM_IMPORT("process__foreground") extern int32_t wasm_import__process_foreground(uint32_t id);
BRUCE_WASM_IMPORT("process__signal") extern int32_t wasm_import__process_signal(uint32_t id, int32_t signal);
BRUCE_WASM_IMPORT("process__terminate") extern int32_t wasm_import__process_terminate(uint32_t id);
BRUCE_WASM_IMPORT("process__pause") extern int32_t wasm_import__process_pause(uint32_t id);
BRUCE_WASM_IMPORT("process__resume") extern int32_t wasm_import__process_resume(uint32_t id);
BRUCE_WASM_IMPORT("process__kill") extern int32_t wasm_import__process_kill(uint32_t id);
BRUCE_WASM_IMPORT("process__wait") extern int32_t wasm_import__process_wait(uint32_t id, uint32_t timeout_ms);

bruce_process_id_t process__current_id(void) { return wasm_import__process_current_id(); }

bruce_process_signal_t process__current_signal(void) {
    return (bruce_process_signal_t)wasm_import__process_current_signal();
}

bruce_result_t process__switch_next(void) { return (bruce_result_t)wasm_import__process_switch_next(); }

bruce_result_t process__switch_previous(void) {
    return (bruce_result_t)wasm_import__process_switch_previous();
}

bruce_result_t process__to_background(void) { return (bruce_result_t)wasm_import__process_to_background(); }

bruce_result_t process__to_foreground(void) { return (bruce_result_t)wasm_import__process_to_foreground(); }

bruce_result_t process__foreground(bruce_process_id_t process_id) {
    return (bruce_result_t)wasm_import__process_foreground(process_id);
}

bruce_result_t process__signal(bruce_process_id_t process_id, bruce_process_signal_t signal) {
    return (bruce_result_t)wasm_import__process_signal(process_id, (int32_t)signal);
}

bruce_result_t process__terminate(bruce_process_id_t process_id) {
    return (bruce_result_t)wasm_import__process_terminate(process_id);
}

bruce_result_t process__pause(bruce_process_id_t process_id) {
    return (bruce_result_t)wasm_import__process_pause(process_id);
}

bruce_result_t process__resume(bruce_process_id_t process_id) {
    return (bruce_result_t)wasm_import__process_resume(process_id);
}

bruce_result_t process__kill(bruce_process_id_t process_id) {
    return (bruce_result_t)wasm_import__process_kill(process_id);
}

bruce_result_t process__wait(bruce_process_id_t process_id, uint32_t timeout_ms) {
    return (bruce_result_t)wasm_import__process_wait(process_id, timeout_ms);
}

BRUCE_WASM_IMPORT("permission__check") extern int32_t wasm_import__permission_check(int32_t permission);

bruce_result_t permission__check(bruce_permission_t permission) {
    return (bruce_result_t)wasm_import__permission_check((int32_t)permission);
}

BRUCE_WASM_IMPORT("stdio__read")
extern int32_t
wasm_import__stdio_read(uint32_t buffer, uint32_t capacity, uint32_t timeout_ms, uint32_t out_size);
BRUCE_WASM_IMPORT("stdio__read_line")
extern int32_t wasm_import__stdio_read_line(uint32_t buffer, uint32_t buffer_size, int32_t mask_input);
BRUCE_WASM_IMPORT("stdio__write") extern int32_t wasm_import__stdio_write(uint32_t data, uint32_t size);
BRUCE_WASM_IMPORT("stdio__session_create")
extern int32_t wasm_import__stdio_session_create(uint32_t out_session);
BRUCE_WASM_IMPORT("stdio__session_close") extern int32_t wasm_import__stdio_session_close(uint32_t session);
BRUCE_WASM_IMPORT("stdio__session_route_children")
extern int32_t wasm_import__stdio_session_route_children(uint32_t session);
BRUCE_WASM_IMPORT("stdio__session_write_input")
extern int32_t wasm_import__stdio_session_write_input(uint32_t session, uint32_t data, uint32_t size);
BRUCE_WASM_IMPORT("stdio__session_read_output")
extern int32_t wasm_import__stdio_session_read_output(
    uint32_t session, uint32_t buffer, uint32_t capacity, uint32_t out_size
);

bruce_result_t stdio__read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size) {
    return (bruce_result_t)wasm_import__stdio_read(
        wasm_bruce_guest_adapter__offset(buffer),
        (uint32_t)capacity,
        timeout_ms,
        wasm_bruce_guest_adapter__offset(out_size)
    );
}

int stdio__read_line(char *buffer, size_t buffer_size, bool mask_input) {
    return wasm_import__stdio_read_line(
        wasm_bruce_guest_adapter__offset(buffer), (uint32_t)buffer_size, mask_input ? 1 : 0
    );
}

bruce_result_t stdio__write(const void *data, size_t size) {
    return (bruce_result_t)wasm_import__stdio_write(wasm_bruce_guest_adapter__offset(data), (uint32_t)size);
}

bruce_result_t stdio__session_create(bruce_stdio_session_t *out_session) {
    return (bruce_result_t)wasm_import__stdio_session_create(wasm_bruce_guest_adapter__offset(out_session));
}

bruce_result_t stdio__session_close(bruce_stdio_session_t session) {
    return (bruce_result_t)wasm_import__stdio_session_close(session);
}

bruce_result_t stdio__session_route_children(bruce_stdio_session_t session) {
    return (bruce_result_t)wasm_import__stdio_session_route_children(session);
}

bruce_result_t stdio__session_write_input(bruce_stdio_session_t session, const void *data, size_t size) {
    return (bruce_result_t)wasm_import__stdio_session_write_input(
        session, wasm_bruce_guest_adapter__offset(data), (uint32_t)size
    );
}

bruce_result_t
stdio__session_read_output(bruce_stdio_session_t session, void *buffer, size_t capacity, size_t *out_size) {
    return (bruce_result_t)wasm_import__stdio_session_read_output(
        session,
        wasm_bruce_guest_adapter__offset(buffer),
        (uint32_t)capacity,
        wasm_bruce_guest_adapter__offset(out_size)
    );
}

BRUCE_WASM_IMPORT("memory__get_stats") extern int32_t wasm_import__memory_get_stats(uint32_t out_stats);

bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats) {
    return (bruce_result_t)wasm_import__memory_get_stats(wasm_bruce_guest_adapter__offset(out_stats));
}

BRUCE_WASM_IMPORT("memory__malloc") extern uint32_t wasm_import__memory_malloc(uint32_t size);
BRUCE_WASM_IMPORT("memory__free") extern void wasm_import__memory_free(uint32_t pointer);

typedef union {
    max_align_t alignment;
    uint32_t size;
} wasm_bruce_guest_allocation_header_t;

void *memory__malloc(size_t size) {
    if (size == 0 || size > UINT32_MAX - sizeof(wasm_bruce_guest_allocation_header_t)) return NULL;
    uint32_t offset = wasm_import__memory_malloc(
        (uint32_t)(sizeof(wasm_bruce_guest_allocation_header_t) + size)
    );
    if (offset == 0) return NULL;
    wasm_bruce_guest_allocation_header_t *header = (void *)(uintptr_t)offset;
    header->size = (uint32_t)size;
    return header + 1;
}

void *memory__calloc(size_t count, size_t size) {
    if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *allocation = memory__malloc(total);
    return allocation != NULL ? memset(allocation, 0, total) : NULL;
}

void *memory__realloc(void *pointer, size_t size) {
    if (pointer == NULL) return memory__malloc(size);
    if (size == 0) {
        memory__free(pointer);
        return NULL;
    }
    wasm_bruce_guest_allocation_header_t *old_header =
        (wasm_bruce_guest_allocation_header_t *)pointer - 1;
    void *allocation = memory__malloc(size);
    if (allocation == NULL) return NULL;
    memcpy(allocation, pointer, old_header->size < size ? old_header->size : size);
    memory__free(pointer);
    return allocation;
}

void memory__free(void *pointer) {
    if (pointer == NULL) return;
    wasm_bruce_guest_allocation_header_t *header =
        (wasm_bruce_guest_allocation_header_t *)pointer - 1;
    wasm_import__memory_free(wasm_bruce_guest_adapter__offset(header));
}

int stdio__vprintf(const char *format, va_list args) {
    if (format == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    va_list measured;
    va_copy(measured, args);
    int length = vsnprintf(NULL, 0, format, measured);
    va_end(measured);
    if (length < 0) return BRUCE_ERR_INTERNAL;
    char *buffer = malloc((size_t)length + 1);
    if (buffer == NULL) return BRUCE_ERR_NO_MEMORY;
    va_list formatted;
    va_copy(formatted, args);
    int written = vsnprintf(buffer, (size_t)length + 1, format, formatted);
    va_end(formatted);
    if (written != length) {
        free(buffer);
        return BRUCE_ERR_INTERNAL;
    }
    bruce_result_t result = stdio__write(buffer, (size_t)length);
    free(buffer);
    return result == BRUCE_OK ? length : result;
}

int stdio__printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = stdio__vprintf(format, args);
    va_end(args);
    return result;
}

#define BRUCE_WASM_IMPORT_I32_0(name) BRUCE_WASM_IMPORT(#name) extern int32_t wasm_import__##name(void)

BRUCE_WASM_IMPORT_I32_0(config__get_time_clock24hr);
BRUCE_WASM_IMPORT_I32_0(config__get_color_primary);
BRUCE_WASM_IMPORT_I32_0(config__get_color_secondary);
BRUCE_WASM_IMPORT_I32_0(config__get_color_background);
BRUCE_WASM_IMPORT_I32_0(config__get_color_surface);
BRUCE_WASM_IMPORT_I32_0(config__get_color_text);
BRUCE_WASM_IMPORT_I32_0(config__get_color_text_muted);
BRUCE_WASM_IMPORT_I32_0(config__get_color_border);
BRUCE_WASM_IMPORT_I32_0(config__get_color_success);
BRUCE_WASM_IMPORT_I32_0(config__get_color_warning);
BRUCE_WASM_IMPORT_I32_0(config__get_color_error);
BRUCE_WASM_IMPORT_I32_0(display__width);
BRUCE_WASM_IMPORT_I32_0(display__height);
BRUCE_WASM_IMPORT_I32_0(display__begin_frame);
BRUCE_WASM_IMPORT_I32_0(display__present);
BRUCE_WASM_IMPORT_I32_0(input__flush);

bool config__get_time_clock24hr(void) { return wasm_import__config__get_time_clock24hr() != 0; }
uint16_t config__get_color_primary(void) { return (uint16_t)wasm_import__config__get_color_primary(); }
uint16_t config__get_color_secondary(void) { return (uint16_t)wasm_import__config__get_color_secondary(); }
uint16_t config__get_color_background(void) { return (uint16_t)wasm_import__config__get_color_background(); }
uint16_t config__get_color_surface(void) { return (uint16_t)wasm_import__config__get_color_surface(); }
uint16_t config__get_color_text(void) { return (uint16_t)wasm_import__config__get_color_text(); }
uint16_t config__get_color_text_muted(void) { return (uint16_t)wasm_import__config__get_color_text_muted(); }
uint16_t config__get_color_border(void) { return (uint16_t)wasm_import__config__get_color_border(); }
uint16_t config__get_color_success(void) { return (uint16_t)wasm_import__config__get_color_success(); }
uint16_t config__get_color_warning(void) { return (uint16_t)wasm_import__config__get_color_warning(); }
uint16_t config__get_color_error(void) { return (uint16_t)wasm_import__config__get_color_error(); }
int display__width(void) { return wasm_import__display__width(); }
int display__height(void) { return wasm_import__display__height(); }
bruce_result_t display__begin_frame(void) { return (bruce_result_t)wasm_import__display__begin_frame(); }
bruce_result_t display__present(void) { return (bruce_result_t)wasm_import__display__present(); }
bruce_result_t input__flush(void) { return (bruce_result_t)wasm_import__input__flush(); }

BRUCE_WASM_IMPORT("display__fill_screen") extern int32_t wasm_import__display_fill_screen(uint32_t color);
BRUCE_WASM_IMPORT("display__draw_rect")
extern int32_t wasm_import__display_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
BRUCE_WASM_IMPORT("display__set_text_bg_color")
extern int32_t wasm_import__display_set_text_bg_color(uint32_t color);
BRUCE_WASM_IMPORT("display__set_text_color") extern int32_t wasm_import__display_set_text_color(uint32_t color);
BRUCE_WASM_IMPORT("display__set_text_size") extern int32_t wasm_import__display_set_text_size(uint32_t size);
BRUCE_WASM_IMPORT("display__draw_centre_string")
extern int32_t wasm_import__display_draw_centre_string(uint32_t text, int32_t x, int32_t y);
BRUCE_WASM_IMPORT("display__get_font_metrics")
extern int32_t wasm_import__display_get_font_metrics(uint32_t out_char_width, uint32_t out_char_height);
BRUCE_WASM_IMPORT("clock__get_local") extern int32_t wasm_import__clock_get_local(uint32_t output);
BRUCE_WASM_IMPORT("process__snapshot")
extern int32_t wasm_import__process_snapshot(uint32_t process_id, uint32_t output);
BRUCE_WASM_IMPORT("input__wait") extern int32_t wasm_import__input_wait(uint32_t timeout_ms, uint32_t output);
BRUCE_WASM_IMPORT("dialog__message")
extern int32_t wasm_import__dialog_message(int32_t kind, uint32_t title, uint32_t message);
BRUCE_WASM_IMPORT("dialog__choice")
extern int32_t wasm_import__dialog_choice(
    uint32_t title, uint32_t message, uint32_t choices, uint32_t count, uint32_t selected
);
BRUCE_WASM_IMPORT("dialog__number_input")
extern int32_t wasm_import__dialog_number_input(
    uint32_t title, uint32_t prompt, uint32_t initial, uint32_t buffer, uint32_t buffer_size
);

bruce_result_t display__fill_screen(bruce_display_color_t color) {
    return (bruce_result_t)wasm_import__display_fill_screen(color);
}
bruce_result_t display__draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    return (bruce_result_t)wasm_import__display_draw_rect(x, y, w, h, color);
}
bruce_result_t display__set_text_bg_color(uint32_t color) {
    return (bruce_result_t)wasm_import__display_set_text_bg_color(color);
}
bruce_result_t display__set_text_color(bruce_display_color_t color) {
    return (bruce_result_t)wasm_import__display_set_text_color(color);
}
bruce_result_t display__set_text_size(uint8_t size) {
    return (bruce_result_t)wasm_import__display_set_text_size(size);
}
bruce_result_t display__draw_centre_string(const char *text, int16_t x, int16_t y) {
    return (bruce_result_t)wasm_import__display_draw_centre_string(
        wasm_bruce_guest_adapter__offset(text), x, y
    );
}
bruce_result_t display__get_font_metrics(int16_t *out_char_width, int16_t *out_char_height) {
    return (bruce_result_t)wasm_import__display_get_font_metrics(
        wasm_bruce_guest_adapter__offset(out_char_width),
        wasm_bruce_guest_adapter__offset(out_char_height)
    );
}
bruce_result_t clock__get_local(bruce_clock_datetime_t *output) {
    return (bruce_result_t)wasm_import__clock_get_local(wasm_bruce_guest_adapter__offset(output));
}
bruce_result_t process__snapshot(bruce_process_id_t process_id, bruce_process_snapshot_t *output) {
    return (bruce_result_t)wasm_import__process_snapshot(
        process_id, wasm_bruce_guest_adapter__offset(output)
    );
}
bruce_result_t input__wait(uint32_t timeout_ms, int32_t *output) {
    return (bruce_result_t)wasm_import__input_wait(timeout_ms, wasm_bruce_guest_adapter__offset(output));
}
bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    return (bruce_result_t)wasm_import__dialog_message(
        (int32_t)kind, wasm_bruce_guest_adapter__offset(title), wasm_bruce_guest_adapter__offset(message)
    );
}
bruce_result_t dialog__choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    return (bruce_result_t)wasm_import__dialog_choice(
        wasm_bruce_guest_adapter__offset(title),
        wasm_bruce_guest_adapter__offset(message),
        wasm_bruce_guest_adapter__offset(choices),
        (uint32_t)choice_count,
        wasm_bruce_guest_adapter__offset(out_selected)
    );
}
bruce_result_t dialog__number_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
) {
    return (bruce_result_t)wasm_import__dialog_number_input(
        wasm_bruce_guest_adapter__offset(title),
        wasm_bruce_guest_adapter__offset(prompt),
        wasm_bruce_guest_adapter__offset(initial_text),
        wasm_bruce_guest_adapter__offset(buffer),
        (uint32_t)buffer_size
    );
}

/* NES compatibility imports. The host-side binding owns all guest range
 * validation; these wrappers only lower the public SDK ABI to wasm32 values. */
BRUCE_WASM_IMPORT("storage__open") extern int32_t wasm_import__storage_open(uint32_t, uint32_t, uint32_t);
BRUCE_WASM_IMPORT("storage__read") extern int32_t wasm_import__storage_read(uint32_t, uint32_t, uint32_t, uint32_t);
BRUCE_WASM_IMPORT("storage__write") extern int32_t wasm_import__storage_write(uint32_t, uint32_t, uint32_t, uint32_t);
BRUCE_WASM_IMPORT("storage__seek") extern int32_t wasm_import__storage_seek(uint32_t, int64_t, int32_t, uint32_t);
BRUCE_WASM_IMPORT("storage__close") extern int32_t wasm_import__storage_close(uint32_t);
BRUCE_WASM_IMPORT("dialog__pick_file") extern int32_t wasm_import__dialog_pick_file(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
BRUCE_WASM_IMPORT("display__game_mode") extern int32_t wasm_import__display_game_mode(int32_t);
BRUCE_WASM_IMPORT("display__color565") extern int32_t wasm_import__display_color565(int32_t, int32_t, int32_t);
BRUCE_WASM_IMPORT("display__fill_rect") extern int32_t wasm_import__display_fill_rect(int32_t, int32_t, int32_t, int32_t, int32_t);
BRUCE_WASM_IMPORT("display__draw_rgb_bitmap") extern int32_t wasm_import__display_draw_rgb_bitmap(int32_t, int32_t, uint32_t, int32_t, int32_t);
BRUCE_WASM_IMPORT("input__read") extern int32_t wasm_import__input_read(uint32_t, uint32_t);
BRUCE_WASM_IMPORT("audio__stream_sample_rate") extern int32_t wasm_import__audio_stream_sample_rate(void);
BRUCE_WASM_IMPORT("audio__stream_open") extern int32_t wasm_import__audio_stream_open(int32_t);
BRUCE_WASM_IMPORT("audio__stream_write") extern int32_t wasm_import__audio_stream_write(uint32_t, uint32_t);
BRUCE_WASM_IMPORT("audio__stream_close") extern int32_t wasm_import__audio_stream_close(void);
BRUCE_WASM_IMPORT("runtime__timer_start") extern int32_t wasm_import__runtime_timer_start(uint32_t, uint32_t, uint32_t);
BRUCE_WASM_IMPORT("runtime__timer_wait") extern int32_t wasm_import__runtime_timer_wait(uint32_t, uint32_t);
BRUCE_WASM_IMPORT("runtime__timer_stop") extern int32_t wasm_import__runtime_timer_stop(uint32_t);

bruce_result_t storage__open(const char *path, uint32_t flags, bruce_file_id_t *out_file) {
    return (bruce_result_t)wasm_import__storage_open(wasm_bruce_guest_adapter__offset(path), flags,
                                                       wasm_bruce_guest_adapter__offset(out_file));
}
bruce_result_t storage__read(bruce_file_id_t file, void *buffer, size_t capacity, size_t *out_size) {
    return (bruce_result_t)wasm_import__storage_read(file, wasm_bruce_guest_adapter__offset(buffer),
                                                       (uint32_t)capacity, wasm_bruce_guest_adapter__offset(out_size));
}
bruce_result_t storage__write(bruce_file_id_t file, const void *buffer, size_t size, size_t *out_size) {
    return (bruce_result_t)wasm_import__storage_write(file, wasm_bruce_guest_adapter__offset(buffer),
                                                       (uint32_t)size, wasm_bruce_guest_adapter__offset(out_size));
}
bruce_result_t storage__seek(bruce_file_id_t file, int64_t offset, int whence, uint64_t *out_position) {
    return (bruce_result_t)wasm_import__storage_seek(file, offset, whence, wasm_bruce_guest_adapter__offset(out_position));
}
bruce_result_t storage__close(bruce_file_id_t file) { return (bruce_result_t)wasm_import__storage_close(file); }
bruce_result_t dialog__pick_file(const char *initial, const char *filter, char *path, size_t size, const char *title) {
    return (bruce_result_t)wasm_import__dialog_pick_file(
        wasm_bruce_guest_adapter__offset(initial), wasm_bruce_guest_adapter__offset(filter),
        wasm_bruce_guest_adapter__offset(path), (uint32_t)size, wasm_bruce_guest_adapter__offset(title)
    );
}
bruce_result_t display__game_mode(bool enabled) { return (bruce_result_t)wasm_import__display_game_mode(enabled); }
bruce_display_color_t display__color565(uint8_t r, uint8_t g, uint8_t b) {
    return (bruce_display_color_t)wasm_import__display_color565(r, g, b);
}
bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    return (bruce_result_t)wasm_import__display_fill_rect(x, y, w, h, color);
}
bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    return (bruce_result_t)wasm_import__display_draw_rgb_bitmap(x, y, wasm_bruce_guest_adapter__offset(bitmap), w, h);
}
bruce_result_t input__read(bruce_input_event_t *event, uint32_t timeout) {
    return (bruce_result_t)wasm_import__input_read(wasm_bruce_guest_adapter__offset(event), timeout);
}
uint32_t audio__stream_sample_rate(void) { return (uint32_t)wasm_import__audio_stream_sample_rate(); }
bruce_result_t audio__stream_open(uint8_t channels) { return (bruce_result_t)wasm_import__audio_stream_open(channels); }
size_t audio__stream_write(const int16_t *samples, size_t frames) {
    return (size_t)wasm_import__audio_stream_write(wasm_bruce_guest_adapter__offset(samples), (uint32_t)frames);
}
bruce_result_t audio__stream_close(void) { return (bruce_result_t)wasm_import__audio_stream_close(); }
bruce_result_t runtime__timer_start(uint32_t period, volatile uint32_t *counter, uint32_t *timer) {
    return (bruce_result_t)wasm_import__runtime_timer_start(period, wasm_bruce_guest_adapter__offset(counter),
                                                             wasm_bruce_guest_adapter__offset(timer));
}
bruce_result_t runtime__timer_wait(uint32_t timer, uint32_t timeout) {
    return (bruce_result_t)wasm_import__runtime_timer_wait(timer, timeout);
}
bruce_result_t runtime__timer_stop(uint32_t timer) { return (bruce_result_t)wasm_import__runtime_timer_stop(timer); }

bruce_result_t memory__external_alloc(size_t size, bruce_memory_object_t *object) {
    (void)size; (void)object; return BRUCE_ERR_UNSUPPORTED;
}
bruce_result_t memory__external_write(const bruce_memory_object_t *object, size_t offset, const void *data, size_t size) {
    (void)object; (void)offset; (void)data; (void)size; return BRUCE_ERR_UNSUPPORTED;
}
bruce_result_t memory__external_map(const bruce_memory_object_t *object, const void **data) {
    (void)object; (void)data; return BRUCE_ERR_UNSUPPORTED;
}
bruce_result_t memory__external_free(bruce_memory_object_t *object) {
    (void)object; return BRUCE_ERR_UNSUPPORTED;
}
