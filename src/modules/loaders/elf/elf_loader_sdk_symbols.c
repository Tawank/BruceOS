/* Public SDK symbol table exported to ELF applications.
 *
 * The ELF loader module registers this table with the Espressif ELF loader and
 * uses a custom resolver that searches only these symbols. Selected libc names
 * are mapped to process-aware SDK functions; all other unknown symbols resolve to
 * 0 and cause relocation failure, which is the desired sandbox behavior.
 *
 * When adding a new public SDK capability, also export its entry points here
 * if ELF apps are expected to call them directly.
 */

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_elf.h" // IWYU pragma: export

#include "core_sdk/app_runner.h"
#include "core_sdk/args.h"
#include "core_sdk/audio.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/bluetooth_hid.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/dialog.h"
#include "core_sdk/disk.h"
#include "core_sdk/display.h"
#include "core_sdk/environment.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/gpio.h"
#include "core_sdk/http.h"
#include "core_sdk/i2c.h"
#include "core_sdk/icon.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/ir.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/notification.h"
#include "core_sdk/nrf24.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/pubsub.h"
#include "core_sdk/runtime.h"
#include "core_sdk/spi.h"
#include "core_sdk/ssh.h"
#include "core_sdk/status_icon.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tcp.h"
#include "core_sdk/tty.h"
#include "core_sdk/wifi.h"

/* GCC emits these libgcc helpers for floating-point operations in ELF apps.
 * Keep them in the restricted resolver so portable C code does not need to
 * carry target-specific libgcc objects inside every loadable image. */
extern int __eqdf2(double left, double right);
extern double __adddf3(double left, double right);
extern long long __divdi3(long long dividend, long long divisor);
extern int *__errno(void);
extern float __divsf3(float left, float right);
extern float __addsf3(float left, float right);
extern float __subsf3(float left, float right);
extern float __mulsf3(float left, float right);
extern float __floatsisf(int value);
extern int __fixsfsi(float value);
extern unsigned int __fixunssfsi(float value);
extern int __gesf2(float left, float right);
extern int __ltsf2(float left, float right);
extern long long __fixdfdi(double value);
extern double __floatsidf(int value);
extern float __floatdisf(long long value);
extern double __floatdidf(long long value);
extern double __floatundidf(unsigned long long value);
extern float __floatundisf(unsigned long long value);
extern double __floatunsidf(unsigned int value);
extern double __extendsfdf2(float value);
extern int __fixdfsi(double value);
extern unsigned int __fixunsdfsi(double value);
extern unsigned long long __fixunsdfdi(double value);
extern long long __fixsfdi(float value);
extern unsigned long long __fixunssfdi(float value);
extern int __gedf2(double left, double right);
extern int __gtdf2(double left, double right);
extern int __ledf2(double left, double right);
extern int __ltdf2(double left, double right);
extern long long __moddi3(long long dividend, long long divisor);
extern int __nedf2(double left, double right);
extern int __unorddf2(double left, double right);
extern unsigned long long __umoddi3(unsigned long long dividend, unsigned long long divisor);
extern double __divdf3(double left, double right);
extern double __muldf3(double left, double right);
extern double __subdf3(double left, double right);
extern float __truncdfsf2(double value);
extern unsigned long long __udivdi3(unsigned long long dividend, unsigned long long divisor);
extern float __ieee754_sqrtf(float value);

static int bruce_elf__puts(const char *text) {
    if (text == NULL || stdio__write(text, strlen(text)) != BRUCE_OK) return EOF;
    return stdio__write("\n", 1) == BRUCE_OK ? 0 : EOF;
}

static int bruce_elf__putchar(int character) {
    unsigned char byte = (unsigned char)character;
    return stdio__write(&byte, 1) == BRUCE_OK ? byte : EOF;
}

/* ---------------------------------------------------------------------------
 * Minimal C++ ABI support, added for C++ ELF apps (see
 * native_apps/examples/game3d, the first one). project_elf() builds ELF apps
 * with `-nostdlib` (see components/elf_loader/elf_loader.cmake) and never
 * links libstdc++/libsupc++, so a C++ app leaves every C++ runtime symbol --
 * even plain `new`/`delete` -- as an unresolved relocation for this loader to
 * satisfy at load time, exactly like malloc/free above.
 *
 * Only the bare minimum is provided: freestanding global operator new/delete
 * (routed through the same process-aware memory__* allocator as malloc/free)
 * and a __cxa_pure_virtual trap. There is deliberately no support for
 * exceptions (__cxa_throw, __gxx_personality_v0, ...), RTTI (typeinfo,
 * dynamic_cast), thread-safe function-local statics (__cxa_guard_*), or
 * global/namespace-scope objects with non-trivial constructors (this loader
 * never walks .init_array -- grep esp_elf.c). A C++ ELF app's own build must
 * therefore compile with -fno-exceptions -fno-rtti -fno-threadsafe-statics
 * and avoid namespace-scope objects with non-trivial constructors; see
 * native_apps/examples/game3d/main/CMakeLists.txt for the flags and
 * native_apps/examples/game3d/README.md for the reasoning.
 *
 * The symbol names below are the Itanium C++ ABI mangling for global
 * operator new/delete on the 32-bit targets this project builds for
 * (size_t == unsigned int, mangled 'j'); a 64-bit target would need the 'm'
 * forms instead. Like malloc/free, a failed allocation returns NULL rather
 * than throwing (there is no exception support to throw with, and nothing
 * in the ELF apps built against this table checks new's return value).
 * ------------------------------------------------------------------------- */
static void *bruce_elf__operator_new(size_t size) { return memory__malloc(size); }
static void bruce_elf__operator_delete(void *ptr) { memory__free(ptr); }
static void bruce_elf__operator_delete_sized(void *ptr, size_t size) {
    (void)size;
    memory__free(ptr);
}
/* A function-local `static` object with a non-trivial destructor (e.g. a
 * std::vector -- see Jet's Scene.cpp, a dependency of
 * native_apps/examples/game3d, the first C++ ELF app) still gets its destructor
 * registered via __cxa_atexit even under -fno-threadsafe-statics, which only
 * suppresses the *initialization* guard (__cxa_guard_*), not this. Real
 * __cxa_atexit registers a destructor to run when the process exits via a
 * full C runtime exit() call; ELF apps have no such teardown path (app_main
 * just returns and the loader reclaims the process's memory directly -- see
 * the .init_array comment above), so there is nothing useful to register.
 * A no-op that reports success satisfies the ABI contract without pretending
 * to actually run anything later.
 *
 * Note this covers __cxa_atexit itself but deliberately not its companion
 * __dso_handle: that symbol has hidden ELF visibility by ABI convention, so
 * project_elf()'s -fPIC -shared link refuses to leave it as a runtime-
 * resolved external the way it does every symbol in this table -- it must
 * be defined inside the app's own linked objects instead. See game3d's
 * main.cpp for that definition and a fuller explanation. */
static int bruce_elf__cxa_atexit(void (*destructor)(void *), void *arg, void *dso_handle) {
    (void)destructor;
    (void)arg;
    (void)dso_handle;
    return 0;
}

static void bruce_elf__cxa_pure_virtual(void) {
    static const char message[] = "bruce: pure virtual function call\n";
    stdio__write(message, sizeof(message) - 1);
    /* Should never be reached (see the comment block above); park the
     * process instead of falling through into whatever garbage the caller
     * expected a real override to return. runtime__delay() keeps this from
     * spinning the watchdog while parked. */
    for (;;) { runtime__delay(1000); }
}

/* libstdc++'s <vector>/<new> headers call these on genuinely exceptional
 * conditions (allocation failure, a requested container size past its
 * implementation limit, an overflowing array-new size computation) even
 * under -fno-exceptions -- the header always calls the out-of-line
 * function; whether it throws or aborts is that function's own decision,
 * normally made inside libstdc++.a, which this table stands in for. Since
 * there is no exception support to throw with here (see the comment block
 * above), park the same way __cxa_pure_virtual does -- one of these firing
 * means a real bug (OOM, or a size that should never have been requested),
 * not a recoverable condition. Mangled names: Itanium C++ ABI for
 * std::__throw_bad_alloc(), std::__throw_length_error(char const*), and
 * std::__throw_bad_array_new_length(). */
static void bruce_elf__throw_bad_alloc(void) {
    static const char message[] = "bruce: std::__throw_bad_alloc\n";
    stdio__write(message, sizeof(message) - 1);
    for (;;) { runtime__delay(1000); }
}
static void bruce_elf__throw_length_error(const char *what) {
    stdio__write("bruce: std::__throw_length_error: ", 35);
    if (what != NULL) stdio__write(what, strlen(what));
    stdio__write("\n", 1);
    for (;;) { runtime__delay(1000); }
}
static void bruce_elf__throw_bad_array_new_length(void) {
    static const char message[] = "bruce: std::__throw_bad_array_new_length\n";
    stdio__write(message, sizeof(message) - 1);
    for (;;) { runtime__delay(1000); }
}

/* operator new(size_t, const std::nothrow_t&): returning NULL on failure is
 * exactly the nothrow contract, so this is just operator new without the
 * (absent) throwing behaviour -- no need to see the actual std::nothrow_t
 * tag object, its type is never inspected. Mangled name: 32-bit size_t
 * ('j'), matching the plain operator new forms above. */
static void *bruce_elf__operator_new_nothrow(size_t size, const void *tag) {
    (void)tag;
    return memory__malloc(size);
}

const struct esp_elfsym g_bruce_sdk_elfsyms[] = {
    /* Core runtime / process */
    ESP_ELFSYM_EXPORT(runtime__now),
    ESP_ELFSYM_EXPORT(runtime__sleep),
    ESP_ELFSYM_EXPORT(runtime__delay),
    ESP_ELFSYM_EXPORT(runtime__timer_start),
    ESP_ELFSYM_EXPORT(runtime__timer_wait),
    ESP_ELFSYM_EXPORT(runtime__timer_stop),
    ESP_ELFSYM_EXPORT(runtime__gui_requested),
    ESP_ELFSYM_EXPORT(process__current_id),
    ESP_ELFSYM_EXPORT(process__switch_next),
    ESP_ELFSYM_EXPORT(process__switch_previous),
    ESP_ELFSYM_EXPORT(process__to_background),
    ESP_ELFSYM_EXPORT(process__to_foreground),
    ESP_ELFSYM_EXPORT(process__foreground),
    ESP_ELFSYM_EXPORT(process__signal),
    ESP_ELFSYM_EXPORT(process__terminate),
    ESP_ELFSYM_EXPORT(process__pause),
    ESP_ELFSYM_EXPORT(process__resume),
    ESP_ELFSYM_EXPORT(process__kill),
    ESP_ELFSYM_EXPORT(process__wait),
    ESP_ELFSYM_EXPORT(process__wait_status),
    ESP_ELFSYM_EXPORT(process__current_signal),
    ESP_ELFSYM_EXPORT(process__clear_signal),
    ESP_ELFSYM_EXPORT(process__snapshot),
    ESP_ELFSYM_EXPORT(process__list),

    /* Audio */
    ESP_ELFSYM_EXPORT(audio__tone),
    ESP_ELFSYM_EXPORT(audio__stream_sample_rate),
    ESP_ELFSYM_EXPORT(audio__stream_open),
    ESP_ELFSYM_EXPORT(audio__stream_writable_frames),
    ESP_ELFSYM_EXPORT(audio__stream_write),
    ESP_ELFSYM_EXPORT(audio__stream_close),

    /* Device state */
    ESP_ELFSYM_EXPORT(device__get_battery),
    ESP_ELFSYM_EXPORT(device__restart),
    ESP_ELFSYM_EXPORT(device__power_off),

    /* Wall clock */
    ESP_ELFSYM_EXPORT(clock__get_utc),
    ESP_ELFSYM_EXPORT(clock__get_local),
    ESP_ELFSYM_EXPORT(clock__set_local),
    ESP_ELFSYM_EXPORT(clock__sync_ntp),
    ESP_ELFSYM_EXPORT(clock__get_sync_status),
    ESP_ELFSYM_EXPORT(clock__get_ntp_server),

    /* Read-only application preferences. Protected values enforce config
     * permission in Core. */
    ESP_ELFSYM_EXPORT(config__get_color_primary),
    ESP_ELFSYM_EXPORT(config__get_color_secondary),
    ESP_ELFSYM_EXPORT(config__get_color_background),
    ESP_ELFSYM_EXPORT(config__get_color_surface),
    ESP_ELFSYM_EXPORT(config__get_color_text),
    ESP_ELFSYM_EXPORT(config__get_color_text_muted),
    ESP_ELFSYM_EXPORT(config__get_color_border),
    ESP_ELFSYM_EXPORT(config__get_color_success),
    ESP_ELFSYM_EXPORT(config__get_color_warning),
    ESP_ELFSYM_EXPORT(config__get_color_error),
    ESP_ELFSYM_EXPORT(config__get_time_clock24hr),

    /* AppRunner / loader */
    ESP_ELFSYM_EXPORT(app_runner__run),
    ESP_ELFSYM_EXPORT(app_runner__run_path),
    ESP_ELFSYM_EXPORT(app_runner__run_with_environment),
    ESP_ELFSYM_EXPORT(app_runner__run_path_with_environment),
    ESP_ELFSYM_EXPORT(ext_mem_loader__stage_path),
    ESP_ELFSYM_EXPORT(ext_mem_loader__adopt_image),
    ESP_ELFSYM_EXPORT(ext_mem_loader__release_image),
    ESP_ELFSYM_EXPORT(app_runner__parse_args),
    ESP_ELFSYM_EXPORT(app_runner__free_args),
    ESP_ELFSYM_EXPORT(app_runner__environment_requests_gui),
    ESP_ELFSYM_EXPORT(app_runner__spawn_loader_process),
    ESP_ELFSYM_EXPORT(app_runner__spawn_loader_process_owned),
    ESP_ELFSYM_EXPORT(app_runner__spawn_loader_process_owned_with_stop),
    ESP_ELFSYM_EXPORT(app_runner__run_command),
    ESP_ELFSYM_EXPORT(app_runner__icon_for_path),

    /* Runtime dependencies used by statically linked external interpreters. */
    ESP_ELFSYM_EXPORT(pthread_self),
    ESP_ELFSYM_EXPORT(pthread_mutex_init),
    ESP_ELFSYM_EXPORT(pthread_mutex_destroy),
    ESP_ELFSYM_EXPORT(pthread_mutex_lock),
    ESP_ELFSYM_EXPORT(pthread_mutex_unlock),
    ESP_ELFSYM_EXPORT(bsearch),
    ESP_ELFSYM_EXPORT(qsort),
    ESP_ELFSYM_EXPORT(__errno),
    ESP_ELFSYM_EXPORT(__ieee754_sqrtf),

    /* Argument parser */
    ESP_ELFSYM_EXPORT(ap_new_parser),
    ESP_ELFSYM_EXPORT(ap_free),
    ESP_ELFSYM_EXPORT(ap_set_helptext),
    ESP_ELFSYM_EXPORT(ap_get_helptext),
    ESP_ELFSYM_EXPORT(ap_set_version),
    ESP_ELFSYM_EXPORT(ap_get_version),
    ESP_ELFSYM_EXPORT(ap_parse),
    ESP_ELFSYM_EXPORT(ap_get_status),
    ESP_ELFSYM_EXPORT(ap_print_help),
    ESP_ELFSYM_EXPORT(ap_first_pos_arg_ends_option_parsing),
    ESP_ELFSYM_EXPORT(ap_all_args_as_pos_args),
    ESP_ELFSYM_EXPORT(ap_allow_extra_args),
    ESP_ELFSYM_EXPORT(ap_unknown_options_as_args),
    ESP_ELFSYM_EXPORT(ap_add_flag),
    ESP_ELFSYM_EXPORT(ap_add_str_opt),
    ESP_ELFSYM_EXPORT(ap_add_int_opt),
    ESP_ELFSYM_EXPORT(ap_add_dbl_opt),
    ESP_ELFSYM_EXPORT(ap_add_greedy_str_opt),
    ESP_ELFSYM_EXPORT(ap_set_opt_help),
    ESP_ELFSYM_EXPORT(ap_count),
    ESP_ELFSYM_EXPORT(ap_found),
    ESP_ELFSYM_EXPORT(ap_get_str_value),
    ESP_ELFSYM_EXPORT(ap_get_str_value_at_index),
    ESP_ELFSYM_EXPORT(ap_get_str_values),
    ESP_ELFSYM_EXPORT(ap_get_int_value),
    ESP_ELFSYM_EXPORT(ap_get_int_value_at_index),
    ESP_ELFSYM_EXPORT(ap_get_int_values),
    ESP_ELFSYM_EXPORT(ap_get_dbl_value),
    ESP_ELFSYM_EXPORT(ap_get_dbl_value_at_index),
    ESP_ELFSYM_EXPORT(ap_get_dbl_values),
    ESP_ELFSYM_EXPORT(ap_add_required_arg),
    ESP_ELFSYM_EXPORT(ap_add_optional_arg),
    ESP_ELFSYM_EXPORT(ap_get_arg),
    ESP_ELFSYM_EXPORT(ap_has_args),
    ESP_ELFSYM_EXPORT(ap_count_args),
    ESP_ELFSYM_EXPORT(ap_get_arg_at_index),
    ESP_ELFSYM_EXPORT(ap_get_args),
    ESP_ELFSYM_EXPORT(ap_get_args_as_ints),
    ESP_ELFSYM_EXPORT(ap_get_args_as_doubles),
    ESP_ELFSYM_EXPORT(ap_new_cmd),
    ESP_ELFSYM_EXPORT(ap_set_cmd_callback),
    ESP_ELFSYM_EXPORT(ap_found_cmd),
    ESP_ELFSYM_EXPORT(ap_get_cmd_name),
    ESP_ELFSYM_EXPORT(ap_get_cmd_parser),
    ESP_ELFSYM_EXPORT(ap_get_cmd_exit_code),
    ESP_ELFSYM_EXPORT(ap_enable_help_command),
    ESP_ELFSYM_EXPORT(ap_get_parent),
    ESP_ELFSYM_EXPORT(ap_print),
    ESP_ELFSYM_EXPORT(ap_had_memory_error),
    ESP_ELFSYM_EXPORT(ap_get_zeroth_root_arg),

    /* Process environment */
    ESP_ELFSYM_EXPORT(environment__global_get),
    ESP_ELFSYM_EXPORT(environment__global_set),
    ESP_ELFSYM_EXPORT(environment__global_unset),
    ESP_ELFSYM_EXPORT(environment__get),
    ESP_ELFSYM_EXPORT(environment__set),
    ESP_ELFSYM_EXPORT(environment__unset),
    ESP_ELFSYM_EXPORT(environment__count),
    ESP_ELFSYM_EXPORT(environment__get_at),

    /* Memory */
    ESP_ELFSYM_EXPORT(memory__malloc),
    ESP_ELFSYM_EXPORT(memory__calloc),
    ESP_ELFSYM_EXPORT(memory__realloc),
    ESP_ELFSYM_EXPORT(memory__free),
    ESP_ELFSYM_EXPORT(memory__external_malloc),
    ESP_ELFSYM_EXPORT(memory__external_malloc_writable),
    ESP_ELFSYM_EXPORT(memory__external_calloc),
    ESP_ELFSYM_EXPORT(memory__external_memcpy),
    ESP_ELFSYM_EXPORT(memory__external_memset),
    ESP_ELFSYM_EXPORT(memory__external_free),
    ESP_ELFSYM_EXPORT(memory__get_stats),
    ESP_ELFSYM_EXPORT(memory__get_layout),
    ESP_ELFSYM_EXPORT(memory__reclaim),
    {"malloc",  (const void *)&memory__malloc    },
    {"calloc",  (const void *)&memory__calloc    },
    {"realloc", (const void *)&memory__realloc   },
    {"free",    (const void *)&memory__free      },

    /* Permission (introspection only; protected APIs check internally) */
    ESP_ELFSYM_EXPORT(permission__check),
    ESP_ELFSYM_EXPORT(permission__from_name),
    ESP_ELFSYM_EXPORT(permission__name),

    /* Wi-Fi and HTTP client */
    ESP_ELFSYM_EXPORT(wifi__disconnect),
    ESP_ELFSYM_EXPORT(wifi__connect),
    ESP_ELFSYM_EXPORT(wifi__connect_known),
    ESP_ELFSYM_EXPORT(wifi__setup_ap),
    ESP_ELFSYM_EXPORT(wifi__scan),
    ESP_ELFSYM_EXPORT(wifi__scan_start),
    ESP_ELFSYM_EXPORT(wifi__scan_poll),
    ESP_ELFSYM_EXPORT(wifi__scan_cancel),
    ESP_ELFSYM_EXPORT(wifi__is_connected),
    ESP_ELFSYM_EXPORT(wifi__is_ap_running),
    ESP_ELFSYM_EXPORT(wifi__get_ssid),
    ESP_ELFSYM_EXPORT(wifi__get_ip),
    ESP_ELFSYM_EXPORT(wifi__get_mac),
    ESP_ELFSYM_EXPORT(http__request),
    ESP_ELFSYM_EXPORT(http__response_free),

    /* Input (read is foreground-only; inject requires input permission) */
    ESP_ELFSYM_EXPORT(input__read),
    ESP_ELFSYM_EXPORT(input__poll),
    ESP_ELFSYM_EXPORT(input__flush),
    ESP_ELFSYM_EXPORT(input__peek),
    ESP_ELFSYM_EXPORT(input__wait),
    ESP_ELFSYM_EXPORT(input__check),
    ESP_ELFSYM_EXPORT(input__inject),

    /* Bluetooth advertisement scan and Classic HID host */
    ESP_ELFSYM_EXPORT(bluetooth__scan_ble),
    ESP_ELFSYM_EXPORT(bluetooth__scan_start),
    ESP_ELFSYM_EXPORT(bluetooth__scan_poll),
    ESP_ELFSYM_EXPORT(bluetooth__scan_cancel),
    ESP_ELFSYM_EXPORT(bluetooth_hid__is_supported),
    ESP_ELFSYM_EXPORT(bluetooth_hid__scan),
    ESP_ELFSYM_EXPORT(bluetooth_hid__connect),
    ESP_ELFSYM_EXPORT(bluetooth_hid__disconnect),
    ESP_ELFSYM_EXPORT(bluetooth_hid__is_connected),
    ESP_ELFSYM_EXPORT(bluetooth_hid__connected_device),

    /* Infrared */
    ESP_ELFSYM_EXPORT(ir__transmit_raw),
    ESP_ELFSYM_EXPORT(ir__transmit),
    ESP_ELFSYM_EXPORT(ir__transmit_parsed),
    ESP_ELFSYM_EXPORT(ir__receive),
    ESP_ELFSYM_EXPORT(ir__transmit_file),
    ESP_ELFSYM_EXPORT(ir__transmit_record),
    ESP_ELFSYM_EXPORT(ir__tx_pin),
    ESP_ELFSYM_EXPORT(ir__rx_pin),

    /* NRF24 passive radio operations */
    ESP_ELFSYM_EXPORT(nrf24__probe),
    ESP_ELFSYM_EXPORT(nrf24__set_channel),
    ESP_ELFSYM_EXPORT(nrf24__get_channel),
    ESP_ELFSYM_EXPORT(nrf24__scan),
    ESP_ELFSYM_EXPORT(nrf24__get_pins),

    /* GPIO and serial buses */
    ESP_ELFSYM_EXPORT(gpio__configure),
    ESP_ELFSYM_EXPORT(gpio__read),
    ESP_ELFSYM_EXPORT(gpio__write),
    ESP_ELFSYM_EXPORT(i2c__open),
    ESP_ELFSYM_EXPORT(i2c__probe),
    ESP_ELFSYM_EXPORT(i2c__write),
    ESP_ELFSYM_EXPORT(i2c__read),
    ESP_ELFSYM_EXPORT(i2c__write_read),
    ESP_ELFSYM_EXPORT(i2c__close),
    ESP_ELFSYM_EXPORT(spi__open),
    ESP_ELFSYM_EXPORT(spi__transfer),
    ESP_ELFSYM_EXPORT(spi__close),

    /* Named-topic pub/sub (e.g. BRUCE_DEVICE_TOPIC_TOUCH) */
    ESP_ELFSYM_EXPORT(pubsub__publish),
    ESP_ELFSYM_EXPORT(pubsub__subscribe),
    ESP_ELFSYM_EXPORT(pubsub__unsubscribe),
    ESP_ELFSYM_EXPORT(pubsub__read),

    /* Display (layout management remains built-in-only) */
    ESP_ELFSYM_EXPORT(display__width),
    ESP_ELFSYM_EXPORT(display__height),
    ESP_ELFSYM_EXPORT(display__color565),
    ESP_ELFSYM_EXPORT(display__fill_screen),
    ESP_ELFSYM_EXPORT(display__clear),
    ESP_ELFSYM_EXPORT(display__set_text_color),
    ESP_ELFSYM_EXPORT(display__set_text_bg_color),
    ESP_ELFSYM_EXPORT(display__set_text_size),
    ESP_ELFSYM_EXPORT(display__set_cursor),
    ESP_ELFSYM_EXPORT(display__get_cursor),
    ESP_ELFSYM_EXPORT(display__get_font_metrics),
    ESP_ELFSYM_EXPORT(display__print),
    ESP_ELFSYM_EXPORT(display__println),
    ESP_ELFSYM_EXPORT(display__draw_string),
    ESP_ELFSYM_EXPORT(display__draw_centre_string),
    ESP_ELFSYM_EXPORT(display__draw_right_string),
    ESP_ELFSYM_EXPORT(display__draw_pixel),
    ESP_ELFSYM_EXPORT(display__draw_line),
    ESP_ELFSYM_EXPORT(display__draw_rect),
    ESP_ELFSYM_EXPORT(display__fill_rect),
    ESP_ELFSYM_EXPORT(display__draw_circle),
    ESP_ELFSYM_EXPORT(display__fill_circle),
    ESP_ELFSYM_EXPORT(display__draw_arc),
    ESP_ELFSYM_EXPORT(display__draw_triangle),
    ESP_ELFSYM_EXPORT(display__fill_triangle),
    ESP_ELFSYM_EXPORT(display__draw_round_rect),
    ESP_ELFSYM_EXPORT(display__fill_round_rect),
    ESP_ELFSYM_EXPORT(display__draw_bitmap),
    ESP_ELFSYM_EXPORT(display__draw_xbitmap),
    ESP_ELFSYM_EXPORT(display__draw_rgb_bitmap),
    ESP_ELFSYM_EXPORT(display__draw_bitmap_scaled),
    ESP_ELFSYM_EXPORT(display__set_rotation),
    ESP_ELFSYM_EXPORT(display__get_rotation),
    ESP_ELFSYM_EXPORT(display__invert_display),
    ESP_ELFSYM_EXPORT(display__set_brightness),
    ESP_ELFSYM_EXPORT(display__get_brightness),
    ESP_ELFSYM_EXPORT(display__display_on_off),
    ESP_ELFSYM_EXPORT(display__request_render_mode),
    ESP_ELFSYM_EXPORT(display__release_render_mode),
    ESP_ELFSYM_EXPORT(display__buffer_footprint),
    ESP_ELFSYM_EXPORT(display__begin_frame),
    ESP_ELFSYM_EXPORT(display__present),
    ESP_ELFSYM_EXPORT(display__screen_width),
    ESP_ELFSYM_EXPORT(display__screen_height),

    /* Overlays: a small always-on-top drawing surface any process may
     * create for its own menu/notification/HUD (see core_sdk/display.h). */
    ESP_ELFSYM_EXPORT(display__overlay_create),
    ESP_ELFSYM_EXPORT(display__overlay_destroy),
    ESP_ELFSYM_EXPORT(display__overlay_show),
    ESP_ELFSYM_EXPORT(display__overlay_hide),
    ESP_ELFSYM_EXPORT(display__overlay_move),
    ESP_ELFSYM_EXPORT(display__overlay_begin),
    ESP_ELFSYM_EXPORT(display__overlay_end),

    /* Built-in vector icons */
    ESP_ELFSYM_EXPORT(icon__get),

    /* Encoded images */
    ESP_ELFSYM_EXPORT(image__get_bitmap_from_memory),
    ESP_ELFSYM_EXPORT(image__get_bitmap_from_file),
    ESP_ELFSYM_EXPORT(image__bitmap_resize),
    ESP_ELFSYM_EXPORT(image__draw_path),
    ESP_ELFSYM_EXPORT(image__draw_bitmap),
    ESP_ELFSYM_EXPORT(image__bitmap_release),
    ESP_ELFSYM_EXPORT(image__is_supported_path),
    ESP_ELFSYM_EXPORT(image__gif_open),
    ESP_ELFSYM_EXPORT(image__gif_draw),
    ESP_ELFSYM_EXPORT(image__gif_increment),
    ESP_ELFSYM_EXPORT(image__gif_close),

    /* Unrestricted global UI services */
    ESP_ELFSYM_EXPORT(notification__push),
    ESP_ELFSYM_EXPORT(notification__dismiss),
    ESP_ELFSYM_EXPORT(status_icon__push),
    ESP_ELFSYM_EXPORT(status_icon__push_named),
    ESP_ELFSYM_EXPORT(status_icon__remove),
    ESP_ELFSYM_EXPORT(status_icon__list),
    ESP_ELFSYM_EXPORT(status_icon__get),

    /* Dialog */
    ESP_ELFSYM_EXPORT(dialog__message),
    ESP_ELFSYM_EXPORT(dialog__choice),
    ESP_ELFSYM_EXPORT(dialog__choice_launcher),
    ESP_ELFSYM_EXPORT(dialog__choice_ex),
    ESP_ELFSYM_EXPORT(dialog__pick_file),
    ESP_ELFSYM_EXPORT(dialog__text_input),
    ESP_ELFSYM_EXPORT(dialog__hex_input),
    ESP_ELFSYM_EXPORT(dialog__number_input),
    ESP_ELFSYM_EXPORT(dialog__create_text_viewer),
    ESP_ELFSYM_EXPORT(dialog__viewer_set_text),
    ESP_ELFSYM_EXPORT(dialog__viewer_set_text_size),
    ESP_ELFSYM_EXPORT(dialog__viewer_scroll),
    ESP_ELFSYM_EXPORT(dialog__viewer_close),

    /* Manifest inspection */
    ESP_ELFSYM_EXPORT(manifest__parse),
    ESP_ELFSYM_EXPORT(manifest__inspect_path),
    ESP_ELFSYM_EXPORT(manifest__inspect_elf),
    ESP_ELFSYM_EXPORT(manifest__inspect_javascript),
    ESP_ELFSYM_EXPORT(manifest__inspect_wasm),

    /* Storage */
    ESP_ELFSYM_EXPORT(storage__open),
    ESP_ELFSYM_EXPORT(storage__read),
    ESP_ELFSYM_EXPORT(storage__write),
    ESP_ELFSYM_EXPORT(storage__seek),
    ESP_ELFSYM_EXPORT(storage__close),
    ESP_ELFSYM_EXPORT(storage__list),
    ESP_ELFSYM_EXPORT(storage__mkdir),
    ESP_ELFSYM_EXPORT(storage__exists),
    ESP_ELFSYM_EXPORT(storage__remove),
    ESP_ELFSYM_EXPORT(storage__rename),
    ESP_ELFSYM_EXPORT(storage__copy),
    ESP_ELFSYM_EXPORT(storage__get_usage),

    /* Block devices */
    ESP_ELFSYM_EXPORT(disk__list),
    ESP_ELFSYM_EXPORT(disk__mount),
    ESP_ELFSYM_EXPORT(disk__unmount),

    /* TCP and console streams */
    ESP_ELFSYM_EXPORT(tcp__connect),
    ESP_ELFSYM_EXPORT(tcp__listen),
    ESP_ELFSYM_EXPORT(tcp__accept),
    ESP_ELFSYM_EXPORT(tcp__read),
    ESP_ELFSYM_EXPORT(tcp__write),
    ESP_ELFSYM_EXPORT(tcp__close),
    ESP_ELFSYM_EXPORT(ssh__connect),
    ESP_ELFSYM_EXPORT(ssh__host_key_sha256),
    ESP_ELFSYM_EXPORT(ssh__verify_host_key_sha256),
    ESP_ELFSYM_EXPORT(ssh__authenticate_password),
    ESP_ELFSYM_EXPORT(ssh__generate_keypair_ex),
    ESP_ELFSYM_EXPORT(ssh__authenticate_key),
    ESP_ELFSYM_EXPORT(ssh__open_shell),
    ESP_ELFSYM_EXPORT(ssh__resize_pty),
    ESP_ELFSYM_EXPORT(ssh__read),
    ESP_ELFSYM_EXPORT(ssh__write),
    ESP_ELFSYM_EXPORT(ssh__close),
    ESP_ELFSYM_EXPORT(stdio__read),
    ESP_ELFSYM_EXPORT(stdio__read_line),
    ESP_ELFSYM_EXPORT(stdio__write),
    ESP_ELFSYM_EXPORT(stdio__printf),
    ESP_ELFSYM_EXPORT(stdio__vprintf),
    ESP_ELFSYM_EXPORT(stdio__session_create),
    ESP_ELFSYM_EXPORT(stdio__session_close),
    ESP_ELFSYM_EXPORT(stdio__session_route_children),
    ESP_ELFSYM_EXPORT(stdio__session_write_input),
    ESP_ELFSYM_EXPORT(stdio__session_read_output),
    ESP_ELFSYM_EXPORT(tty__isatty),
    ESP_ELFSYM_EXPORT(tty__get_size),
    ESP_ELFSYM_EXPORT(tty__set_size),
    ESP_ELFSYM_EXPORT(tty__get_mode),
    ESP_ELFSYM_EXPORT(tty__set_mode),

    /* Standard C library subset. Console and heap calls are routed through
     * process-aware Bruce SDK functions rather than firmware libc. */
    {"printf",  (const void *)&stdio__printf     },
    {"vprintf", (const void *)&stdio__vprintf    },
    {"puts",    (const void *)&bruce_elf__puts   },
    {"putchar", (const void *)&bruce_elf__putchar},
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(sprintf),
    ESP_ELFSYM_EXPORT(vsnprintf),
    ESP_ELFSYM_EXPORT(sscanf),
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strcat),
    ESP_ELFSYM_EXPORT(strncat),
    ESP_ELFSYM_EXPORT(strchr),
    ESP_ELFSYM_EXPORT(strrchr),
    ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(strtol),
    ESP_ELFSYM_EXPORT(strtoll),
    ESP_ELFSYM_EXPORT(strtoul),
    ESP_ELFSYM_EXPORT(strtoull),
    ESP_ELFSYM_EXPORT(atoi),
    ESP_ELFSYM_EXPORT(atol),
    ESP_ELFSYM_EXPORT(atoll),
    ESP_ELFSYM_EXPORT(abs),
    ESP_ELFSYM_EXPORT(labs),
    ESP_ELFSYM_EXPORT(llabs),

    /* GCC runtime helpers used by freestanding ELF code. */
    ESP_ELFSYM_EXPORT(__eqdf2),
    ESP_ELFSYM_EXPORT(__adddf3),
    ESP_ELFSYM_EXPORT(__divdi3),
    ESP_ELFSYM_EXPORT(__divsf3),
    ESP_ELFSYM_EXPORT(__addsf3),
    ESP_ELFSYM_EXPORT(__subsf3),
    ESP_ELFSYM_EXPORT(__mulsf3),
    ESP_ELFSYM_EXPORT(__floatsisf),
    ESP_ELFSYM_EXPORT(__fixsfsi),
    ESP_ELFSYM_EXPORT(__fixunssfsi),
    ESP_ELFSYM_EXPORT(__gesf2),
    ESP_ELFSYM_EXPORT(__ltsf2),
    ESP_ELFSYM_EXPORT(__fixdfdi),
    ESP_ELFSYM_EXPORT(__floatsidf),
    ESP_ELFSYM_EXPORT(__floatdisf),
    ESP_ELFSYM_EXPORT(__floatdidf),
    ESP_ELFSYM_EXPORT(__floatundidf),
    ESP_ELFSYM_EXPORT(__floatundisf),
    ESP_ELFSYM_EXPORT(__floatunsidf),
    ESP_ELFSYM_EXPORT(__extendsfdf2),
    ESP_ELFSYM_EXPORT(__fixdfsi),
    ESP_ELFSYM_EXPORT(__fixunsdfsi),
    ESP_ELFSYM_EXPORT(__fixunsdfdi),
    ESP_ELFSYM_EXPORT(__fixsfdi),
    ESP_ELFSYM_EXPORT(__fixunssfdi),
    ESP_ELFSYM_EXPORT(__gedf2),
    ESP_ELFSYM_EXPORT(__gtdf2),
    ESP_ELFSYM_EXPORT(__ledf2),
    ESP_ELFSYM_EXPORT(__ltdf2),
    ESP_ELFSYM_EXPORT(__moddi3),
    ESP_ELFSYM_EXPORT(__nedf2),
    ESP_ELFSYM_EXPORT(__unorddf2),
    ESP_ELFSYM_EXPORT(__umoddi3),
    ESP_ELFSYM_EXPORT(__divdf3),
    ESP_ELFSYM_EXPORT(__muldf3),
    ESP_ELFSYM_EXPORT(__subdf3),
    ESP_ELFSYM_EXPORT(__truncdfsf2),
    ESP_ELFSYM_EXPORT(__udivdi3),

    /* libm. Real implementations already exist in the firmware's own libm;
     * this just exposes them to ELF apps that do floating-point math
     * (e.g. native_apps/examples/game3d's 3D renderer). */
    ESP_ELFSYM_EXPORT(sin),
    ESP_ELFSYM_EXPORT(cos),
    ESP_ELFSYM_EXPORT(tan),
    ESP_ELFSYM_EXPORT(sinf),
    ESP_ELFSYM_EXPORT(cosf),
    ESP_ELFSYM_EXPORT(tanf),
    ESP_ELFSYM_EXPORT(sqrt),
    ESP_ELFSYM_EXPORT(sqrtf),
    ESP_ELFSYM_EXPORT(atan2),
    ESP_ELFSYM_EXPORT(atan2f),
    ESP_ELFSYM_EXPORT(fabsf),
    ESP_ELFSYM_EXPORT(floor),
    ESP_ELFSYM_EXPORT(ceil),
    ESP_ELFSYM_EXPORT(fmodf),

    /* C++ freestanding new/delete + pure-virtual trap (see the C++ ABI
     * comment block above). Mangled names, not ESP_ELFSYM_EXPORT: these are
     * C++ operators, not C symbols the preprocessor can name directly. */
    {"_Znwj", (const void *)&bruce_elf__operator_new}, /* operator new(size_t) */
    {"_Znaj", (const void *)&bruce_elf__operator_new}, /* operator new[](size_t) */
    {"_ZdlPv", (const void *)&bruce_elf__operator_delete}, /* operator delete(void*) */
    {"_ZdaPv", (const void *)&bruce_elf__operator_delete}, /* operator delete[](void*) */
    {"_ZdlPvj", (const void *)&bruce_elf__operator_delete_sized}, /* operator delete(void*, size_t) */
    {"_ZdaPvj", (const void *)&bruce_elf__operator_delete_sized}, /* operator delete[](void*, size_t) */
    {"__cxa_pure_virtual", (const void *)&bruce_elf__cxa_pure_virtual},
    {"__cxa_atexit", (const void *)&bruce_elf__cxa_atexit},
    {"_ZnwjRKSt9nothrow_t", (const void *)&bruce_elf__operator_new_nothrow}, /* operator new(size_t, const std::nothrow_t&) */
    {"_ZSt17__throw_bad_allocv", (const void *)&bruce_elf__throw_bad_alloc}, /* std::__throw_bad_alloc() */
    {"_ZSt20__throw_length_errorPKc", (const void *)&bruce_elf__throw_length_error}, /* std::__throw_length_error(const char*) */
    {"_ZSt28__throw_bad_array_new_lengthv", (const void *)&bruce_elf__throw_bad_array_new_length}, /* std::__throw_bad_array_new_length() */

    ESP_ELFSYM_END,
};
