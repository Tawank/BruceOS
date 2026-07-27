/* Public SDK symbol table exported to ELF applications.
 *
 * The ELF loader module registers this table with the Espressif ELF loader and
 * uses a custom resolver that searches only these symbols.  Imported libc
 * malloc/free are explicitly rejected; all other unknown symbols resolve to 0
 * and cause relocation failure, which is the desired sandbox behavior.
 *
 * When adding a new public SDK capability, also export its entry points here
 * if ELF apps are expected to call them directly.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_elf.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/bluetooth_hid.h"
#include "core_sdk/dialog.h"
#include "core_sdk/device.h"
#include "core_sdk/display.h"
#include "core_sdk/gpio.h"
#include "core_sdk/i2c.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/ir.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/notification.h"
#include "core_sdk/nrf24.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/spi.h"
#include "core_sdk/storage.h"
#include "core_sdk/status_icon.h"
#include "core_sdk/stdio.h"
#include "core_sdk/task.h"
#include "core_sdk/tcp.h"

const struct esp_elfsym g_bruce_sdk_elfsyms[] = {
    /* Core runtime / task */
    ESP_ELFSYM_EXPORT(runtime__now),
    ESP_ELFSYM_EXPORT(runtime__sleep),
    ESP_ELFSYM_EXPORT(runtime__delay),
    ESP_ELFSYM_EXPORT(task__current_id),
    ESP_ELFSYM_EXPORT(task__to_background),
    ESP_ELFSYM_EXPORT(task__foreground),
    ESP_ELFSYM_EXPORT(task__stop),
    ESP_ELFSYM_EXPORT(task__pause),
    ESP_ELFSYM_EXPORT(task__resume),
    ESP_ELFSYM_EXPORT(task__kill),
    ESP_ELFSYM_EXPORT(task__wait),
    ESP_ELFSYM_EXPORT(task__snapshot),
    ESP_ELFSYM_EXPORT(task__list),

    /* Device state */
    ESP_ELFSYM_EXPORT(device__get_battery),
    ESP_ELFSYM_EXPORT(device__get_time),
    ESP_ELFSYM_EXPORT(device__get_date),

    /* AppRunner / loader */
    ESP_ELFSYM_EXPORT(app_runner__run_path),
    ESP_ELFSYM_EXPORT(app_runner__parse_args),
    ESP_ELFSYM_EXPORT(app_runner__free_args),
    ESP_ELFSYM_EXPORT(app_runner__args_have_gui),

    /* Memory */
    ESP_ELFSYM_EXPORT(memory__malloc),
    ESP_ELFSYM_EXPORT(memory__free),

    /* Permission (introspection only; protected APIs check internally) */
    ESP_ELFSYM_EXPORT(permission__check),
    ESP_ELFSYM_EXPORT(permission__from_name),
    ESP_ELFSYM_EXPORT(permission__name),

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
    ESP_ELFSYM_EXPORT(display__print),
    ESP_ELFSYM_EXPORT(display__println),
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
    ESP_ELFSYM_EXPORT(display__set_rotation),
    ESP_ELFSYM_EXPORT(display__get_rotation),
    ESP_ELFSYM_EXPORT(display__invert_display),
    ESP_ELFSYM_EXPORT(display__set_brightness),
    ESP_ELFSYM_EXPORT(display__get_brightness),
    ESP_ELFSYM_EXPORT(display__display_on_off),
    ESP_ELFSYM_EXPORT(display__begin_frame),
    ESP_ELFSYM_EXPORT(display__present),
    ESP_ELFSYM_EXPORT(display__flush),

    /* Encoded images */
    ESP_ELFSYM_EXPORT(image__draw_memory),
    ESP_ELFSYM_EXPORT(image__draw_path),
    ESP_ELFSYM_EXPORT(image__is_supported_path),

    /* Unrestricted global UI services */
    ESP_ELFSYM_EXPORT(notification__push),
    ESP_ELFSYM_EXPORT(notification__dismiss),
    ESP_ELFSYM_EXPORT(status_icon__push),
    ESP_ELFSYM_EXPORT(status_icon__remove),
    ESP_ELFSYM_EXPORT(status_icon__list),

    /* Dialog */
    ESP_ELFSYM_EXPORT(dialog__message),
    ESP_ELFSYM_EXPORT(dialog__choice),
    ESP_ELFSYM_EXPORT(dialog__pick_file),
    ESP_ELFSYM_EXPORT(dialog__text_input),
    ESP_ELFSYM_EXPORT(dialog__hex_input),
    ESP_ELFSYM_EXPORT(dialog__number_input),
    ESP_ELFSYM_EXPORT(dialog__create_text_viewer),
    ESP_ELFSYM_EXPORT(dialog__viewer_set_text),
    ESP_ELFSYM_EXPORT(dialog__viewer_scroll),
    ESP_ELFSYM_EXPORT(dialog__viewer_close),

    /* Manifest inspection */
    ESP_ELFSYM_EXPORT(manifest__parse),
    ESP_ELFSYM_EXPORT(manifest__inspect_path),
    ESP_ELFSYM_EXPORT(manifest__inspect_elf),

    /* Storage */
    ESP_ELFSYM_EXPORT(storage__open),
    ESP_ELFSYM_EXPORT(storage__read),
    ESP_ELFSYM_EXPORT(storage__write),
    ESP_ELFSYM_EXPORT(storage__seek),
    ESP_ELFSYM_EXPORT(storage__close),
    ESP_ELFSYM_EXPORT(storage__list),
    ESP_ELFSYM_EXPORT(storage__mkdir),

    /* TCP and console streams */
    ESP_ELFSYM_EXPORT(tcp__connect),
    ESP_ELFSYM_EXPORT(tcp__listen),
    ESP_ELFSYM_EXPORT(tcp__accept),
    ESP_ELFSYM_EXPORT(tcp__read),
    ESP_ELFSYM_EXPORT(tcp__write),
    ESP_ELFSYM_EXPORT(tcp__close),
    ESP_ELFSYM_EXPORT(bruce_stdio_read),
    ESP_ELFSYM_EXPORT(bruce_stdio_read_line),
    ESP_ELFSYM_EXPORT(bruce_stdio_session_create),
    ESP_ELFSYM_EXPORT(bruce_stdio_session_close),
    ESP_ELFSYM_EXPORT(bruce_stdio_session_route_children),
    ESP_ELFSYM_EXPORT(bruce_stdio_session_write_input),
    ESP_ELFSYM_EXPORT(bruce_stdio_session_read_output),

    /* Standard C library subset (provided by firmware, not by forwarding
     * malloc/free to libc). */
    ESP_ELFSYM_EXPORT(printf),
    ESP_ELFSYM_EXPORT(puts),
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(sprintf),
    ESP_ELFSYM_EXPORT(vprintf),
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

    ESP_ELFSYM_END,
};
