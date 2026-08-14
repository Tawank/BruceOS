/* This file is compiled into WASM applications, never into the firmware. */

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define BRUCE_WASM_IMPORT(name) __attribute__((import_module("bruce_sdk"), import_name(name)))

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

static uint32_t wasm_bruce_guest_adapter__offset(const void *pointer) { return (uint32_t)(uintptr_t)pointer; }

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
