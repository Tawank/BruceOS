set(BRUCE_SELFTEST_SOURCES
    "modules/selftest/args_test.c"
    "modules/selftest/app_config_test.c"
    "modules/selftest/app_runner_test.c"
    "modules/selftest/archive_test.c"
    "modules/selftest/audio_test.c"
    "modules/selftest/base64_test.c"
    "modules/selftest/bluetooth_test.c"
    "modules/selftest/clock_test.c"
    "modules/selftest/clock_model_test.c"
    "modules/selftest/bnu_test.c"
    "modules/selftest/bnu_sed_test.c"
    "modules/selftest/compress_test.c"
    "modules/selftest/config_test.c"
    "modules/selftest/device_test.c"
    "modules/selftest/dialog_test.c"
    "modules/selftest/display_test.c"
    "modules/selftest/elf_loader_test.c"
    "modules/selftest/elf_loader_xip_fixture.c"
    "modules/selftest/fake_elf.c"
    "modules/selftest/filemanager_network_test.c"
    "modules/selftest/filemanager_pathicons_test.c"
    "modules/selftest/filetype_test.c"
    "modules/selftest/gpio_bus_test.c"
    "modules/selftest/gps_test.c"
    "modules/selftest/appdata_test.c"
    "modules/selftest/cellular_test.c"
    "modules/selftest/core_launcher_test.c"
    "modules/selftest/hash_test.c"
    "modules/selftest/html_test.c"
    "modules/selftest/icmp_test.c"
    "modules/selftest/icon_test.c"
    "modules/selftest/image_test.c"
    "modules/selftest/input_test.c"
    "modules/selftest/ir_test.c"
    "modules/selftest/launcher_test.c"
    "modules/selftest/loader_test.c"
    "modules/selftest/man_test.c"
    "modules/selftest/memory_test.c"
    "modules/selftest/net_test.c"
    "modules/selftest/notification_test.c"
    "modules/selftest/nrf24_test.c"
    "modules/selftest/partition_manager_test.c"
    "modules/selftest/clipboard_test.c"
    "modules/selftest/permission_test.c"
    "modules/selftest/selftest.c"
    "modules/selftest/storage_test.c"
    "modules/selftest/process_test.c"
    "modules/selftest/shell_test.c"
    "modules/selftest/ssh_sftp_test.c"
    "modules/selftest/stdio_test.c"
    "modules/selftest/terminal_test.c"
    "modules/selftest/udp_test.c"
    "modules/selftest/wasm_bruce_sdk_test.c"
    "modules/selftest/wifi_test.c")

# BLE scanning's host-stack backend: NimBLE on every BLE-only target (see
# boards/*/sdkconfig.defaults.esp32s3), Bluedroid where Classic BT HID host
# (bluetooth_hid.c) also needs it. Mirrors the same choice in ESP-IDF's own
# esp_hid/CMakeLists.txt.
if(CONFIG_BT_NIMBLE_ENABLED)
    set(BRUCE_BLUETOOTH_BACKEND_SRC "core/bluetooth/bluetooth_nimble.c")
else()
    set(BRUCE_BLUETOOTH_BACKEND_SRC "core/bluetooth/bluetooth_bluedroid.c")
endif()

# Selftests only in QEMU test builds; the WebAssembly loader only when built
# in (see Kconfig.projbuild). main.c registers their commands to match.
set(BRUCE_OPTIONAL_SOURCES)
if(CONFIG_BRUCE_QEMU_TEST_MODE)
    list(APPEND BRUCE_OPTIONAL_SOURCES ${BRUCE_SELFTEST_SOURCES})
endif()
if(CONFIG_BRUCE_WASM_BUILTIN)
    list(APPEND BRUCE_OPTIONAL_SOURCES
         "modules/loaders/wasm/wasm_loader_app.c"
         "modules/loaders/wasm/wasm_bruce_host_adapter.c")
endif()

idf_component_register(SRCS
                    "main.c"
                     "core/app_config/app_config.c"
                     "core/app_runner/app_runner.c"
                     "core/appdata/appdata.c"
                     "core/archive/archive.c"
                     "core/archive/archive_zip.c"
                     "core/audio/audio.c"
                    "core/autostart/autostart.c"
                    "core/base64/base64.c"
                    "core/bluetooth/bluetooth.c"
                    "${BRUCE_BLUETOOTH_BACKEND_SRC}"
                    "core/bluetooth/bluetooth_hid.c"
                    "core/cellular/cellular.c"
                    "core/cellular/cmux.c"
                    "core/clock/clock.c"
                    "core/compress/compress.c"
                    "core/config/config.c"
                    "core/dialog/dialog.c"
                    "core/dialog/dialog_choice.c"
                    "core/dialog/dialog_gui_common.c"
                    "core/dialog/dialog_input.c"
                    "core/dialog/dialog_message.c"
                    "core/dialog/dialog_pick_file.c"
                    "core/dialog/dialog_term.c"
                    "core/dialog/dialog_time.c"
                    "core/dialog/dialog_viewer.c"
                    "core/device/board_i2c.c"
                    "core/device/device.c"
                    "core/display/display.c"
                    "core/display/display_draw.c"
                    "core/display/display_driver.c"
                    "core/display/display_epd.c"
                    "core/display/display_font_bitmap.c"
                    "core/display/display_overlay.c"
                    "core/display/display_text.c"
                    "core/disk/disk.c"
                    "core/format/format.c"
                    "core/gpio/gpio.c"
                    "core/gps/gps.c"
                    "core/hash/hash.c"
                    "core/html/html_entities.c"
                    "core/html/html_events.c"
                    "core/html/html_parser.c"
                    "core/html/html_tags.c"
                    "core/html/html_url.c"
                    "core/icmp/icmp.c"
                    "core/icon/icon.c"
                    "core/http/http.c"
                    "core/http/http_async.c"
                    "core/image/image.c"
                    "core/image/gif/gif.c"
                    "core/image/jpg/jpg.c"
                    "core/image/png/png.c"
                    "core/image/reader.c"
                    "core/image/render.c"
                    "core/i2c/i2c.c"
                    "core/event_loop/event_loop.c"
                     "core/ir/ir.c"
                    "core/ext_mem_loader/ext_mem_loader.c"
                    "core/filetype/filetype.c"
                     "core/launcher/launcher.c"
                    "core/lib/lib.c"
                    "core/lvgl/lvgl_port.c"
                    "core/lib/lib_symbols.c"
                     "core/launcher/launcher_tree.c"
                     "core/manifest/manifest.c"
                    "core/memory/memory.c"
                    "core/memory/memory_external.c"
                    "core/memory/memory_layout.c"
                    "core/memory/memory_reclaim.c"
                    "core/memory/memory_rtc.c"
                    "core/net/net.c"
                    "core/network/network.c"
                    "core/notification/notification.c"
                    "core/nrf24/nrf24.c"
                    "core/partition_manager/partition_manager.c"
                    "core/clipboard/clipboard.c"
                    "core/permission/permission.c"
                    "core/pubsub/pubsub.c"
                    "core/result/result.c"
                    "core/stdio/stdio.c"
                     "core/spi/spi.c"
                     "core/ssh/ssh.c"
                    "core/storage/storage.c"
                    "core/status_icon/status_icon.c"
                     "core/process/process.c"
                      "core/process/process_api.c"
                      "core/process/process_environment.c"
                      "core/process/process_stack_guard.c"
                      "core/process/runtime_timer.c"
                    "core/tcp/tcp.c"
                    "core/udp/udp.c"
                    "core/wifi/wifi_common.c"
                     "core/http_server/http_server.c"
                     "modules/apps/apps_app.c"
                     "modules/archive/archive_app.c"
                     "modules/bootanimation/bootanimation_app.c"
                     "modules/bruce_launcher/bruce_launcher_app.c"
                    "modules/bruce_launcher/bruce_launcher_config.c"
                    "modules/bruce_launcher/bruce_launcher_icons.c"
                    "modules/bruce_launcher/bruce_launcher_menu.c"
                    "modules/bruce_launcher/bruce_launcher_menu_editor.c"
                    "modules/bnu/bnu_app.c"
                    "modules/bnu/bnu_fs_app.c"
                    "modules/bnu/bnu_disk_app.c"
                    "modules/bnu/bnu_sys_app.c"
                    "modules/bnu/bnu_net_app.c"
                    "modules/bnu/bnu_pager_app.c"
                    "modules/bnu/bnu_hash_app.c"
                    "modules/bnu/bnu_grep_app.c"
                    "modules/bnu/bnu_sed_app.c"
                    "modules/bnu/bnu_wc_app.c"
                    "modules/bnu/bnu_text_app.c"
                    "modules/bnu/bnu_xxd_app.c"
                    "modules/bnu/bnu_clipboard_app.c"
                    "modules/bnu/bnu_memorydump_app.c"
                    "modules/bnu/bnu_compress_app.c"
                    "modules/bnu/bnu_archive_app.c"
                    "modules/bluetooth/bluetooth_app.c"
                    "modules/bluetooth_hid/bluetooth_hid_app.c"
                    "hal/hal.c"
                    "hal/hal_battery.c"
                    "hal/hal_gpio.c"
                    "hal/hal_hog.c"
                    "hal/hal_i2c.c"
                    "hal/hal_keypad.c"
                    "hal/hal_rail.c"
                    "hal/hal_spi.c"
                    "hal/hal_touch.c"
                    "hal/drivers/battery/bq27220.c"
                    "hal/drivers/gpio/xl9555.c"
                    "hal/drivers/keypad/tca8418.c"
                    "hal/drivers/touch/cst3530.c"
                    "modules/hal/hal_app.c"
                    "modules/clock/clock_app.c"
                    "modules/clock/clock_gui.c"
                    "modules/clock/clock_model.c"
                    "modules/clock/clock_overlay.c"
                    "modules/clock/clock_screens.c"
                    "modules/config/config_app.c"
                    "modules/device_bus/device_bus_app.c"
                     "modules/filemanager/filemanager_app.c"
                     "modules/filemanager/filemanager_actions.c"
                     "modules/filemanager/filemanager_clipboard.c"
                     "modules/filemanager/filemanager_network.c"
                     "modules/filemanager/filemanager_pathicons.c"
                     "modules/browser/browser_app.c"
                     "modules/browser/browser_debug.c"
                     "modules/browser/browser_document.c"
                     "modules/browser/browser_history.c"
                     "modules/browser/browser_image_cache.c"
                     "modules/browser/browser_layout.c"
                     "modules/browser/browser_page.c"
                     "modules/browser/browser_render.c"
                     "modules/input/input_app.c"
                    "modules/input/input_buttons.c"
                    "modules/input/input_encoder.c"
                    "modules/input/input_hotkey.c"
                    "modules/input/input_keyboard.c"
                    "modules/input/input_touch.c"
                     "modules/loaders/elf/elf_loader_app.c"
                     "modules/loaders/elf/elf_loader_sdk_symbols.c"
                     "modules/loaders/image/image_loader_app.c"
                      "modules/loaders/js/audio_js.c"
                     "modules/loaders/js/dialog_js.c"
                    "modules/loaders/js/display_js.c"
                     "modules/loaders/js/icon_js.c"
                     "modules/loaders/js/js_source.c"
                     "modules/loaders/js/js_stdlib.c"
                    "modules/loaders/js/keyboard_js.c"
                    "modules/loaders/js/ir_js.c"
                    "modules/loaders/js/notification_js.c"
                    "modules/loaders/js/js_loader_app.c"
                    "modules/loaders/js/runtime_js.c"
                    "modules/loaders/js/serial_js.c"
                     "modules/loaders/js/wifi_js.c"
                     "modules/shell/shell_app.c"
                     "modules/shell/shell_arith.c"
                     "modules/shell/shell_array.c"
                     "modules/shell/shell_condition.c"
                     "modules/shell/shell_compound.c"
                     "modules/shell/shell_brace.c"
                     "modules/shell/shell_console.c"
                     "modules/shell/shell_glob.c"
                     "modules/shell/shell_history.c"
                     "modules/shell/shell_line_editor.c"
                    "modules/shell/shell_parser.c"
                    "modules/shell/shell_executor.c"
                      "modules/shell/shell_builtins.c"
                      "modules/shell/shell_jobs.c"
                     "modules/system_menu/system_menu_app.c"
                     ${BRUCE_OPTIONAL_SOURCES}
                    "modules/ssh/ssh_app.c"
                    "modules/ssh/ssh_sftp_app.c"
                     "modules/nc/nc_app.c"
                     "modules/nmap/nmap_app.c"
                     "modules/ffplay/ffplay_app.c"
                     "modules/player/player_app.c"
                     "modules/text/text_app.c"
                     "modules/utils/launcher/launcher_app.c"
                     "modules/utils/man/man_app.c"
                     "modules/utils/serial_commands/serial_commands_app.c"
                     "modules/utils/storage_commands/storage_commands_app.c"
                     "modules/utils/terminal/terminal_app.c"
                     "modules/utils/terminal/terminal_ansi.c"
                     "modules/utils/process/process_app.c"
                     "modules/utils/process/process_list.c"
                    "modules/utils/notify/notify_app.c"
                    "modules/notification_service/notification_service.c"
                     "modules/ir/ir_app.c"
                     "modules/ir/ir_tvbgone.c"
                    "modules/nrf24/nrf24_app.c"
                    "modules/gps/gps_app.c"
                    "modules/bparted/bparted_app.c"
                    "modules/bparted/bparted_cli.c"
                    "modules/bparted/bparted_gui.c"
                    "modules/bparted/bparted_common.c"
                    "modules/wifi/wifi_app.c"
                    "modules/speedtest/speedtest_app.c"
                    "modules/webui/webui_app.c"
                    "modules/webui/webui_http.c"
                    "modules/webui/webui_memory.c"
                    "modules/webui/webui_auth.c"
                    "modules/webui/webui_files.c"
                    "modules/webui/webui_system.c"
                    "modules/privileged/app_settings/app_settings_app.c"
                    REQUIRES
                    args
                    ncurses
                    cjson
                    bt
                    esp_event
                    espcoredump
                    driver
                    esp_adc
                    esp_driver_gpio
                     esp_driver_i2c
                     esp_driver_i2s
                     esp_driver_ledc
                    esp_driver_rmt
                    esp_driver_spi
                    esp_driver_uart
                    esp_driver_usb_serial_jtag
                    esp_driver_sdspi
                    esp_http_client
                    esp_http_server
                    esp_lcd
                    esp_lcd_ili9341
                    esp_lcd_touch
                    esp_lcd_touch_ft5x06
                    esp_timer
                    esp_jpeg
                    lvgl
                    esp_hid
                     esp_mm
                     esp_netif
                     esp_partition
                     esp_wifi
                    fatfs
                    log
                    lwip
                    mbedtls
                     nvs_flash
                     spi_flash
                     littlefs
                     libjpeg-turbo
                     libpng
                     zlib
                     wolfssl
                     wolfssh
                     elf_loader
                     microtar
                     minizip
                     mquickjs
                     wasm-micro-runtime
                     esp_audio_simple_player
                    INCLUDE_DIRS
                    "."
                      PRIV_INCLUDE_DIRS
                      ".")

# The board's HAL devices (src/hal/hal.h): boards/<id>/board.c, or none.
set(bruce_board_id "${CONFIG_BRUCE_BOARD_ID}")
if(NOT bruce_board_id)
    idf_build_get_property(bruce_board_id BRUCE_BOARD_ID)
endif()
set(bruce_board_file "${CMAKE_CURRENT_LIST_DIR}/../boards/${bruce_board_id}/board.c")
if(bruce_board_id AND EXISTS "${bruce_board_file}")
    target_sources(${COMPONENT_LIB} PRIVATE "${bruce_board_file}")
else()
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/hal/board_none.c")
endif()

target_link_options(${COMPONENT_LIB} INTERFACE
                    "LINKER:--wrap=jpeg_get_large"
                    "LINKER:--wrap=jpeg_open_backing_store"
                    # Diagnostics for the "task stack is sane" cache asserts;
                    # see core/process/process_stack_guard.c.
                    "LINKER:--wrap=esp_cache_freeze_caches_disable_interrupts"
                    "LINKER:--wrap=spi_flash_disable_interrupts_caches_and_other_cpu"
                    "LINKER:-u,__wrap_esp_cache_freeze_caches_disable_interrupts"
                    "LINKER:-u,__wrap_spi_flash_disable_interrupts_caches_and_other_cpu"
                    "LINKER:-u,__muldi3")

# wolfSSH/wolfSSL build fixups for this ESP-IDF v6 / PSRAM-less / software-crypto
# configuration; see components/wolfssl_esp_patches/patch_wolfssl_esp_idf6.cmake
# for the four upstream bugs this patches, and sdkconfig.defaults for the
# CONFIG_ESP_WOLFSSL_* options this build relies on.
set(WOLFSSL_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../managed_components/wolfssl__wolfssl")
include("${CMAKE_CURRENT_LIST_DIR}/../components/wolfssl_esp_patches/patch_wolfssl_esp_idf6.cmake")

idf_component_get_property(bruce_wolfssl_lib wolfssl__wolfssl COMPONENT_LIB)
# Kconfig's CONFIG_ESP_WOLFSSL_NO_ESP32_CRYPT only takes effect if
# esp32-crypt.h is processed before wolfssl/wolfcrypt/settings.h derives
# WOLFSSL_ESP32_CRYPT from it; settings.h is included first in the normal
# header chain, so force esp32-crypt.h in ahead of everything via -include.
target_compile_options(${bruce_wolfssl_lib} PRIVATE
    -include "${WOLFSSL_COMPONENT_DIR}/wolfssl/wolfcrypt/port/Espressif/esp32-crypt.h")
# wolfSSH calls wc_SSH_KDF(), only compiled into wolfssl when this is defined.
target_compile_definitions(${bruce_wolfssl_lib} PUBLIC WOLFSSL_WOLFSSH WOLFSSL_ED25519_STREAMING_VERIFY)

# wolfSSH's PTY/terminal support (pty-req + shell channel, requested together
# via wolfSSH_SetChannelType(..., WOLFSSH_SESSION_TERMINAL, ...) in
# src/core/ssh/ssh.c) is compiled out unless this is defined. It is further
# gated by `!defined(NO_FILESYSTEM)` upstream (NO_FILESYSTEM is unconditionally
# set for this board in wolfssl__wolfssl's user_settings.h); see
# patch_wolfssh_esp_idf6.cmake for why that half of the guard is dropped.
# NO_TERMIOS skips wolfSSH's CreateMode() branch that mirrors the *local*
# client's tcgetattr(STDIN_FILENO, ...) terminal settings to the server --
# meaningless on a board with no attached host tty -- in favor of its fallback
# fixed-baud-rate mode string.
set(WOLFSSH_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../managed_components/wolfssl__wolfssh")
include("${CMAKE_CURRENT_LIST_DIR}/../components/wolfssl_esp_patches/patch_wolfssh_esp_idf6.cmake")

idf_component_get_property(bruce_wolfssh_lib wolfssl__wolfssh COMPONENT_LIB)
# wolfSSH's SFTP client (src/wolfsftp.c) is entirely `#ifdef WOLFSSH_SFTP`;
# needed for the read-only SFTP browsing in modules/ssh/ssh_sftp_app.c
# (via ssh__sftp_* in src/core/ssh/ssh.c).
target_compile_definitions(${bruce_wolfssh_lib} PRIVATE WOLFSSH_TERM NO_TERMIOS WOLFSSH_SFTP WOLFSSL_ED25519_STREAMING_VERIFY)

# wolfSSH's default per-channel flow-control window (DEFAULT_WINDOW_SZ,
# wolfssh/internal.h) is 128KB, allocated as a single WMALLOC() in ChannelNew()
# -- far larger than this PSRAM-less board's largest free heap block
# (~72KB even with ~120KB total free), so channel creation reliably fails
# with a memory allocation error right after a successful authenticate. Same
# class of problem as MAX_SSH_PACKET_LEN in the old libssh2 backend. 2000 is
# the value wolfSSL's own Espressif user_settings.h template recommends for
# embedded targets (see the ESP_ENABLE_WOLFSSH block there); we set it
# directly rather than opting into that whole Kconfig-gated block.
target_compile_definitions(${bruce_wolfssh_lib} PRIVATE DEFAULT_WINDOW_SZ=2000)

# This component's own sources (core/ssh/ssh.c) include wolfssh/wolfssl headers
# directly and must take the WOLFSSL_USER_SETTINGS_DIR path rather than
# expecting a ./configure-generated wolfssl/options.h that this managed
# component packaging never produces.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WOLFSSL_USER_SETTINGS)

# Each module-local embedded_resources.h (e.g. modules/webui/, modules/
# bruce_launcher/) is generated from tools/generate_embedded_resources.py's
# RESOURCES list -- a plain Python list of (source file under
# embedded_resources/, header it belongs in, gzip?) tuples. Adding or
# changing an embedded resource is a one-line edit to that list; this file
# never needs to change for it. The header/source paths below are queried
# from the script itself (--print-headers/--print-sources) rather than
# duplicated here, so they can't drift out of sync with RESOURCES.
#
# The custom command only reruns when the script or one of the listed
# source files is actually newer than the header it produces (ordinary
# add_custom_command OUTPUT/DEPENDS tracking -- not on every build), and the
# script additionally skips the write when a header's content would be
# byte-identical, so its mtime -- and anything that would recompile because
# of it -- stays untouched.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BRUCE_EMBEDDED_RESOURCES_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_embedded_resources.py")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${BRUCE_EMBEDDED_RESOURCES_SCRIPT}" --print-headers
    OUTPUT_VARIABLE BRUCE_EMBEDDED_RESOURCES_HEADERS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${BRUCE_EMBEDDED_RESOURCES_SCRIPT}" --print-sources
    OUTPUT_VARIABLE BRUCE_EMBEDDED_RESOURCES_SOURCES
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" BRUCE_EMBEDDED_RESOURCES_HEADERS "${BRUCE_EMBEDDED_RESOURCES_HEADERS}")
string(REPLACE "\n" ";" BRUCE_EMBEDDED_RESOURCES_SOURCES "${BRUCE_EMBEDDED_RESOURCES_SOURCES}")
add_custom_command(
    OUTPUT ${BRUCE_EMBEDDED_RESOURCES_HEADERS}
    COMMAND "${Python3_EXECUTABLE}" "${BRUCE_EMBEDDED_RESOURCES_SCRIPT}"
    DEPENDS ${BRUCE_EMBEDDED_RESOURCES_SOURCES}
    COMMENT "Generating embedded_resources.h headers"
    VERBATIM)
add_custom_target(bruce_embedded_resources DEPENDS ${BRUCE_EMBEDDED_RESOURCES_HEADERS})
add_dependencies(${COMPONENT_LIB} bruce_embedded_resources)

# The ELF loader exports LVGL's public API (core_sdk/lvgl.h): every lv_*
# declaration lv_conf.h enables, listed from lvgl.h at build time so the list
# always matches the configuration.
idf_build_get_property(bruce_python PYTHON)
idf_component_get_property(bruce_lvgl_dir lvgl__lvgl COMPONENT_DIR)
set(BRUCE_LVGL_CONF_DIR "${CMAKE_CURRENT_LIST_DIR}/core_sdk/lvgl")
set(BRUCE_LVGL_API "${CMAKE_CURRENT_BINARY_DIR}/lvgl_api.h")
set(BRUCE_LVGL_EXPORTS "${CMAKE_CURRENT_BINARY_DIR}/lvgl_exports.inc")
add_custom_command(
    OUTPUT ${BRUCE_LVGL_EXPORTS}
    COMMAND ${CMAKE_C_COMPILER} -E -P -DLV_KCONFIG_IGNORE -I${BRUCE_LVGL_CONF_DIR} -I${bruce_lvgl_dir}
            ${bruce_lvgl_dir}/lvgl.h -o ${BRUCE_LVGL_API}
    COMMAND ${bruce_python} ${CMAKE_CURRENT_LIST_DIR}/../tools/gen_lvgl_exports.py ${BRUCE_LVGL_API}
            ${BRUCE_LVGL_EXPORTS}
    DEPENDS ${BRUCE_LVGL_CONF_DIR}/lv_conf.h ${CMAKE_CURRENT_LIST_DIR}/../tools/gen_lvgl_exports.py
    COMMENT "Listing LVGL exports"
    VERBATIM)
add_custom_target(bruce_lvgl_exports DEPENDS ${BRUCE_LVGL_EXPORTS})
add_dependencies(${COMPONENT_LIB} bruce_lvgl_exports)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# The SDK check targets below are plain CMake targets, outside the global
# options the root CMakeLists.txt gives components.
idf_component_get_property(bruce_lvgl_lib lvgl__lvgl COMPONENT_LIB)
function(bruce_use_lvgl target)
    target_link_libraries(${target} PRIVATE ${bruce_lvgl_lib})
    target_include_directories(${target} PRIVATE "${BRUCE_LVGL_CONF_DIR}")
    target_compile_definitions(${target} PRIVATE LV_KCONFIG_IGNORE)
endfunction()

set_source_files_properties(${BRUCE_SELFTEST_SOURCES} PROPERTIES
                            COMPILE_OPTIONS
                            "-include;${CMAKE_CURRENT_LIST_DIR}/modules/selftest/selftest_output.h")

# These compile the real built-in module sources against the exported
# core_sdk/ include namespace.  Built-in source must include only
# "core_sdk/..." headers; Core-private includes are rejected by source review.
add_library(bruce_sdk_builtin_launcher_check OBJECT
            "modules/bruce_launcher/bruce_launcher_app.c"
            "modules/bruce_launcher/bruce_launcher_config.c"
            "modules/bruce_launcher/bruce_launcher_icons.c"
            "modules/bruce_launcher/bruce_launcher_menu.c"
            "modules/bruce_launcher/bruce_launcher_menu_editor.c")
add_dependencies(bruce_sdk_builtin_launcher_check bruce_embedded_resources)
target_include_directories(bruce_sdk_builtin_launcher_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bruce_launcher"
                           "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__cjson/cJSON")
# The launcher module legitimately uses the third-party espressif/cjson
# component API; the core_sdk-only check is about rejecting Core-private headers,
# not component dependencies.

add_library(bruce_sdk_builtin_bnu_check OBJECT
            "modules/bnu/bnu_app.c"
            "modules/bnu/bnu_fs_app.c"
            "modules/bnu/bnu_disk_app.c"
            "modules/bnu/bnu_sys_app.c"
            "modules/bnu/bnu_net_app.c"
            "modules/bnu/bnu_pager_app.c"
            "modules/bnu/bnu_hash_app.c"
            "modules/bnu/bnu_grep_app.c"
            "modules/bnu/bnu_sed_app.c"
            "modules/bnu/bnu_wc_app.c"
            "modules/bnu/bnu_text_app.c"
            "modules/bnu/bnu_xxd_app.c"
            "modules/bnu/bnu_clipboard_app.c")
target_sources(bruce_sdk_builtin_bnu_check PRIVATE "modules/bnu/bnu_memorydump_app.c")
target_sources(bruce_sdk_builtin_bnu_check PRIVATE "modules/bnu/bnu_compress_app.c" "modules/bnu/bnu_archive_app.c")
target_include_directories(bruce_sdk_builtin_bnu_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bnu")

add_library(bruce_sdk_builtin_apps_check OBJECT "modules/apps/apps_app.c")
target_include_directories(bruce_sdk_builtin_apps_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/apps")

add_library(bruce_sdk_builtin_archive_check OBJECT "modules/archive/archive_app.c")
target_include_directories(bruce_sdk_builtin_archive_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/archive")

add_library(bruce_sdk_builtin_bootanimation_check OBJECT "modules/bootanimation/bootanimation_app.c")
target_include_directories(bruce_sdk_builtin_bootanimation_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bootanimation")

add_library(bruce_sdk_builtin_input_check OBJECT
           "modules/input/input_app.c"
           "modules/input/input_buttons.c"
           "modules/input/input_encoder.c"
           "modules/input/input_hotkey.c"
           "modules/input/input_keyboard.c"
           "modules/input/input_touch.c")
target_include_directories(bruce_sdk_builtin_input_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/input")
# GPIO and touch access go through core_sdk/gpio.h and core_sdk/pubsub.h only;
# the one remaining direct ESP-IDF dependency is the keyboard matrix scan's
# microsecond settle delay (esp_rom_delay_us), which has no core_sdk
# equivalent and isn't hardware access requiring a permission gate.
target_link_libraries(bruce_sdk_builtin_input_check PRIVATE idf::esp_rom idf::freertos idf::log)

add_library(bruce_sdk_builtin_clock_check OBJECT
            "modules/clock/clock_app.c"
            "modules/clock/clock_gui.c"
            "modules/clock/clock_model.c"
            "modules/clock/clock_overlay.c"
            "modules/clock/clock_screens.c")
target_include_directories(bruce_sdk_builtin_clock_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/clock")
bruce_use_lvgl(bruce_sdk_builtin_clock_check)

add_library(bruce_sdk_builtin_config_check OBJECT "modules/config/config_app.c")
target_include_directories(bruce_sdk_builtin_config_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/config")

add_library(bruce_sdk_builtin_speedtest_check OBJECT "modules/speedtest/speedtest_app.c")
target_include_directories(bruce_sdk_builtin_speedtest_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/speedtest")
bruce_use_lvgl(bruce_sdk_builtin_speedtest_check)

add_library(bruce_sdk_builtin_wifi_check OBJECT "modules/wifi/wifi_app.c")
target_include_directories(bruce_sdk_builtin_wifi_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/wifi")

add_library(bruce_sdk_builtin_webui_check OBJECT
           "modules/webui/webui_app.c"
           "modules/webui/webui_http.c"
           "modules/webui/webui_memory.c"
           "modules/webui/webui_auth.c"
           "modules/webui/webui_files.c"
           "modules/webui/webui_system.c")
add_dependencies(bruce_sdk_builtin_webui_check bruce_embedded_resources)
target_include_directories(bruce_sdk_builtin_webui_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/webui")

add_library(bruce_sdk_builtin_bluetooth_check OBJECT "modules/bluetooth/bluetooth_app.c")
target_include_directories(bruce_sdk_builtin_bluetooth_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bluetooth")

add_library(bruce_sdk_builtin_bluetooth_hid_check OBJECT "modules/bluetooth_hid/bluetooth_hid_app.c")
target_include_directories(bruce_sdk_builtin_bluetooth_hid_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bluetooth_hid")

add_library(bruce_sdk_builtin_ir_check OBJECT
            "modules/ir/ir_app.c"
            "modules/ir/ir_tvbgone.c")
target_include_directories(bruce_sdk_builtin_ir_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/ir")

add_library(bruce_sdk_builtin_nrf24_check OBJECT "modules/nrf24/nrf24_app.c")
target_include_directories(bruce_sdk_builtin_nrf24_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/nrf24")

add_library(bruce_sdk_builtin_gps_check OBJECT "modules/gps/gps_app.c")
target_include_directories(bruce_sdk_builtin_gps_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/gps")

add_library(bruce_sdk_builtin_bparted_check OBJECT
            "modules/bparted/bparted_app.c"
            "modules/bparted/bparted_cli.c"
            "modules/bparted/bparted_gui.c"
            "modules/bparted/bparted_common.c")
target_include_directories(bruce_sdk_builtin_bparted_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bparted")

add_library(bruce_sdk_builtin_filemanager_check OBJECT
            "modules/filemanager/filemanager_app.c"
            "modules/filemanager/filemanager_actions.c"
            "modules/filemanager/filemanager_clipboard.c"
            "modules/filemanager/filemanager_network.c"
            "modules/filemanager/filemanager_pathicons.c")
add_dependencies(bruce_sdk_builtin_filemanager_check bruce_embedded_resources)
target_include_directories(bruce_sdk_builtin_filemanager_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/filemanager"
                           "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__cjson/cJSON")
# filemanager_network.c legitimately uses the third-party espressif/cjson
# component API (same as the launcher/system_menu checks above); the
# core_sdk-only check is about rejecting Core-private headers, not component
# dependencies.

add_library(bruce_sdk_builtin_browser_check OBJECT
            "modules/browser/browser_app.c"
            "modules/browser/browser_debug.c"
            "modules/browser/browser_document.c"
            "modules/browser/browser_history.c"
            "modules/browser/browser_image_cache.c"
            "modules/browser/browser_layout.c"
            "modules/browser/browser_page.c"
            "modules/browser/browser_render.c")
target_include_directories(bruce_sdk_builtin_browser_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/browser")

add_library(bruce_sdk_builtin_image_loader_check OBJECT "modules/loaders/image/image_loader_app.c")
target_include_directories(bruce_sdk_builtin_image_loader_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/loaders/image")

add_library(bruce_sdk_builtin_text_check OBJECT "modules/text/text_app.c")
target_include_directories(bruce_sdk_builtin_text_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/text")

add_library(bruce_sdk_builtin_utils_launcher_check OBJECT "modules/utils/launcher/launcher_app.c")
target_include_directories(bruce_sdk_builtin_utils_launcher_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/launcher")

add_library(bruce_sdk_builtin_utils_man_check OBJECT "modules/utils/man/man_app.c")
target_include_directories(bruce_sdk_builtin_utils_man_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/man")

add_library(bruce_sdk_builtin_utils_terminal_check OBJECT
            "modules/utils/terminal/terminal_app.c"
            "modules/utils/terminal/terminal_ansi.c")
target_include_directories(bruce_sdk_builtin_utils_terminal_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/terminal")

add_library(bruce_sdk_builtin_utils_process_check OBJECT
            "modules/utils/process/process_app.c"
            "modules/utils/process/process_list.c")
target_include_directories(bruce_sdk_builtin_utils_process_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/process")

add_library(bruce_sdk_builtin_utils_serial_commands_check OBJECT
            "modules/utils/serial_commands/serial_commands_app.c")
target_include_directories(bruce_sdk_builtin_utils_serial_commands_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/serial_commands")

add_library(bruce_sdk_builtin_utils_storage_commands_check OBJECT
            "modules/utils/storage_commands/storage_commands_app.c")
target_include_directories(bruce_sdk_builtin_utils_storage_commands_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/storage_commands")

add_library(bruce_sdk_builtin_shell_check OBJECT
            "modules/shell/shell_app.c"
            "modules/shell/shell_arith.c"
            "modules/shell/shell_array.c"
            "modules/shell/shell_condition.c"
            "modules/shell/shell_compound.c"
            "modules/shell/shell_brace.c"
            "modules/shell/shell_console.c"
            "modules/shell/shell_glob.c"
            "modules/shell/shell_history.c"
            "modules/shell/shell_line_editor.c"
            "modules/shell/shell_parser.c"
            "modules/shell/shell_executor.c"
            "modules/shell/shell_builtins.c"
            "modules/shell/shell_jobs.c")
target_include_directories(bruce_sdk_builtin_shell_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/shell")

add_library(bruce_sdk_builtin_notification_check OBJECT "modules/utils/notify/notify_app.c")
target_include_directories(bruce_sdk_builtin_notification_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/notify")

add_library(bruce_sdk_builtin_notification_service_check OBJECT
            "modules/notification_service/notification_service.c")
target_include_directories(bruce_sdk_builtin_notification_service_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/notification_service")

add_library(bruce_sdk_builtin_nc_check OBJECT "modules/nc/nc_app.c")
target_include_directories(bruce_sdk_builtin_nc_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/nc")

add_library(bruce_sdk_builtin_ffplay_check OBJECT "modules/ffplay/ffplay_app.c")
target_include_directories(bruce_sdk_builtin_ffplay_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/ffplay")

add_library(bruce_sdk_builtin_player_check OBJECT "modules/player/player_app.c")
target_include_directories(bruce_sdk_builtin_player_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/player")

add_library(bruce_sdk_builtin_nmap_check OBJECT "modules/nmap/nmap_app.c")
target_include_directories(bruce_sdk_builtin_nmap_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/nmap")

add_library(bruce_sdk_builtin_ssh_check OBJECT "modules/ssh/ssh_app.c")
target_include_directories(bruce_sdk_builtin_ssh_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/ssh")

add_library(bruce_sdk_builtin_ssh_sftp_check OBJECT "modules/ssh/ssh_sftp_app.c")
target_include_directories(bruce_sdk_builtin_ssh_sftp_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/ssh")

add_library(bruce_sdk_builtin_system_menu_check OBJECT "modules/system_menu/system_menu_app.c")
target_include_directories(bruce_sdk_builtin_system_menu_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/system_menu"
                           "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__cjson/cJSON")

add_library(bruce_sdk_builtin_elf_loader_check OBJECT
            "modules/loaders/elf/elf_loader_app.c"
            "modules/loaders/elf/elf_loader_sdk_symbols.c")
target_include_directories(bruce_sdk_builtin_elf_loader_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/loaders/elf"
                           "${CMAKE_CURRENT_LIST_DIR}/../components/elf_loader/include")
# The ELF loader module legitimately uses the third-party espressif/elf_loader
# component API; the core_sdk-only check is about rejecting Core-private headers,
# not component dependencies.
target_link_libraries(bruce_sdk_builtin_elf_loader_check PRIVATE elf_loader)
bruce_use_lvgl(bruce_sdk_builtin_elf_loader_check)
target_include_directories(bruce_sdk_builtin_elf_loader_check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(bruce_sdk_builtin_elf_loader_check bruce_lvgl_exports)

add_library(bruce_sdk_builtin_js_loader_check OBJECT
            "modules/loaders/js/audio_js.c"
            "modules/loaders/js/dialog_js.c"
            "modules/loaders/js/display_js.c"
            "modules/loaders/js/icon_js.c"
            "modules/loaders/js/js_source.c"
            "modules/loaders/js/js_stdlib.c"
            "modules/loaders/js/keyboard_js.c"
            "modules/loaders/js/ir_js.c"
            "modules/loaders/js/notification_js.c"
            "modules/loaders/js/js_loader_app.c"
            "modules/loaders/js/runtime_js.c"
            "modules/loaders/js/serial_js.c"
            "modules/loaders/js/wifi_js.c")
target_include_directories(bruce_sdk_builtin_js_loader_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/loaders/js"
                           "${CMAKE_CURRENT_LIST_DIR}/../components/mquickjs")
target_link_libraries(bruce_sdk_builtin_js_loader_check PRIVATE mquickjs)
# The JS loader check target includes the generated mquickjs stdlib header,
# so make sure the generator runs before it is compiled.
add_dependencies(bruce_sdk_builtin_js_loader_check mquickjs_generate_stdlib)

add_library(bruce_sdk_builtin_wasm_loader_check OBJECT
             "modules/loaders/wasm/wasm_loader_app.c"
             "modules/loaders/wasm/wasm_bruce_host_adapter.c")
idf_component_get_property(wasm_micro_runtime_dir wasm-micro-runtime COMPONENT_DIR)
target_include_directories(bruce_sdk_builtin_wasm_loader_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/loaders/wasm"
                           "${wasm_micro_runtime_dir}/core/iwasm/include")
target_link_libraries(bruce_sdk_builtin_wasm_loader_check PRIVATE wasm-micro-runtime)

set(BRUCE_ARGS_SDK_CHECK_TARGETS
    bruce_sdk_builtin_apps_check
    bruce_sdk_builtin_archive_check
    bruce_sdk_builtin_bnu_check
    bruce_sdk_builtin_clock_check
    bruce_sdk_builtin_config_check
    bruce_sdk_builtin_speedtest_check
    bruce_sdk_builtin_launcher_check
    bruce_sdk_builtin_wifi_check
    bruce_sdk_builtin_webui_check
    bruce_sdk_builtin_bluetooth_check
    bruce_sdk_builtin_bluetooth_hid_check
    bruce_sdk_builtin_ir_check
    bruce_sdk_builtin_nrf24_check
    bruce_sdk_builtin_gps_check
    bruce_sdk_builtin_bparted_check
    bruce_sdk_builtin_browser_check
    bruce_sdk_builtin_image_loader_check
    bruce_sdk_builtin_text_check
    bruce_sdk_builtin_utils_man_check
    bruce_sdk_builtin_utils_process_check
    bruce_sdk_builtin_utils_storage_commands_check
    bruce_sdk_builtin_utils_terminal_check
    bruce_sdk_builtin_shell_check
    bruce_sdk_builtin_notification_check
    bruce_sdk_builtin_nc_check
    bruce_sdk_builtin_nmap_check
    bruce_sdk_builtin_player_check
    bruce_sdk_builtin_ssh_check
    bruce_sdk_builtin_ssh_sftp_check
    bruce_sdk_builtin_system_menu_check
    bruce_sdk_builtin_elf_loader_check
    bruce_sdk_builtin_js_loader_check
    bruce_sdk_builtin_wasm_loader_check)
foreach(target IN LISTS BRUCE_ARGS_SDK_CHECK_TARGETS)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../components/args")
endforeach()

# modules/selftest, modules/device_bus, and modules/privileged/* are
# intentionally NOT added to the core_sdk-only compile check above.
# - selftest is explicitly permitted to include Core-private headers and call
#   FreeRTOS/ESP-IDF directly (see selftest.h and migration_plan.md) so it can
#   validate Core's private internals.
# - device_bus owns the shared board I2C bus via the same Core-private
#   core/device/board_i2c.h that core/display/display_driver.c also depends
#   on for its boot-time (pre-process) backlight init; core_sdk/i2c.h's
#   process-scoped i2c__open() cannot be used at that boot stage, so this
#   module is Core-private by necessity rather than SDK-only like other
#   built-ins.
# - modules/privileged/app_settings reads/writes /config/permissions.json
#   directly via core/storage/storage.h's storage__read_file()/
#   storage__write_file_atomic(); that path is unconditionally refused by the
#   public core_sdk/storage.h storage__open()/storage__list() (see
#   storage__is_protected_path() in core/storage/storage.c), so a permissions
#   management UI has no core_sdk-only way to enumerate saved decisions. Any
#   future modules/privileged/* module inherits the same Core-private
#   exemption for the same class of reason - a built-in that legitimately
#   needs to see something the public SDK deliberately hides from apps.

# Audio codec libraries for /lib (core/lib, core/audio/audio_codec_lib.h),
# built from esp_audio_codec's prebuilt archive into <build>/lib.
# tools/build_libs.py links with Xtensa options.
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    idf_build_get_property(bruce_python PYTHON)
    idf_build_get_property(bruce_build_dir BUILD_DIR)
    idf_build_get_property(bruce_target IDF_TARGET)
    idf_component_get_property(bruce_audio_codec_dir espressif__esp_audio_codec COMPONENT_DIR)
    add_custom_target(bruce_libs ALL
        COMMAND ${bruce_python} ${CMAKE_CURRENT_LIST_DIR}/../tools/build_libs.py
                --cc ${CMAKE_C_COMPILER}
                --codec-dir ${bruce_audio_codec_dir}
                --target ${bruce_target}
                --include ${CMAKE_CURRENT_LIST_DIR}/core/audio
                --symbols ${CMAKE_CURRENT_LIST_DIR}/core/lib/lib_symbols.c
                --out ${bruce_build_dir}/lib
        BYPRODUCTS ${bruce_build_dir}/lib/g711_${bruce_target}.so
        VERBATIM)
    # QEMU selftest loads a real library: the small G.711 one, embedded.
    if(CONFIG_BRUCE_QEMU_TEST_MODE)
        target_add_binary_data(${COMPONENT_LIB} ${bruce_build_dir}/lib/g711_${bruce_target}.so BINARY
                               RENAME_TO selftest_g711_lib DEPENDS bruce_libs)
    endif()
endif()
