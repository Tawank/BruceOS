set(BRUCE_SELFTEST_SOURCES
    "modules/selftest/args_test.c"
    "modules/selftest/app_runner_test.c"
    "modules/selftest/bluetooth_test.c"
    "modules/selftest/clock_test.c"
    "modules/selftest/bnu_test.c"
    "modules/selftest/config_test.c"
    "modules/selftest/device_test.c"
    "modules/selftest/dialog_test.c"
    "modules/selftest/display_test.c"
    "modules/selftest/fake_elf.c"
    "modules/selftest/gpio_bus_test.c"
    "modules/selftest/icon_test.c"
    "modules/selftest/image_test.c"
    "modules/selftest/input_test.c"
    "modules/selftest/ir_test.c"
    "modules/selftest/launcher_test.c"
    "modules/selftest/loader_test.c"
    "modules/selftest/notification_test.c"
    "modules/selftest/nrf24_test.c"
    "modules/selftest/permission_test.c"
    "modules/selftest/selftest.c"
    "modules/selftest/storage_test.c"
    "modules/selftest/process_test.c"
    "modules/selftest/shell_test.c"
    "modules/selftest/terminal_test.c"
    "modules/selftest/wifi_test.c")

idf_component_register(SRCS
                    "main.c"
                    "core/app_runner/app_runner.c"
                    "core/bluetooth/bluetooth.c"
                    "core/bluetooth/bluetooth_hid.c"
                    "core/clock/clock.c"
                    "core/config/config.c"
                    "core/dialog/dialog.c"
                    "core/device/device.c"
                    "core/display/display.c"
                    "core/display/display_draw.c"
                    "core/display/display_driver.c"
                    "core/display/display_text.c"
                    "core/gpio/gpio.c"
                    "core/icon/icon.c"
                    "core/http/http.c"
                    "core/image/image.c"
                    "core/i2c/i2c.c"
                    "core/input/input.c"
                     "core/ir/ir.c"
                    "core/loader_image/loader_image.c"
                     "core/manifest/manifest.c"
                    "core/memory/memory.c"
                    "core/network/network.c"
                    "core/notification/notification.c"
                    "core/nrf24/nrf24.c"
                    "core/permission/permission.c"
                    "core/stdio/stdio.c"
                     "core/spi/spi.c"
                     "core/ssh/ssh.c"
                    "core/storage/storage.c"
                    "core/status_icon/status_icon.c"
                    "core/process/process.c"
                    "core/tcp/tcp.c"
                    "core/wifi/wifi_common.c"
                     "core/http_server/http_server.c"
                     "modules/apps/apps_app.c"
                     "modules/bruce_launcher/bruce_launcher_app.c"
                    "modules/bruce_launcher/bruce_launcher_icons.c"
                    "modules/bruce_launcher/bruce_launcher_menu.c"
                    "modules/bnu/bnu_app.c"
                    "modules/bluetooth/bluetooth_app.c"
                    "modules/bluetooth_hid/bluetooth_hid_app.c"
                    "modules/clock/clock_app.c"
                    "modules/config/config_app.c"
                    "modules/filemanager/filemanager_app.c"
                    "modules/loaders/elf/elf_loader_app.c"
                    "modules/loaders/elf/elf_loader_sdk_symbols.c"
                    "modules/loaders/image/image_loader_app.c"
                    "modules/loaders/js/dialog_js.c"
                    "modules/loaders/js/display_js.c"
                    "modules/loaders/js/icon_js.c"
                    "modules/loaders/js/js_stdlib.c"
                    "modules/loaders/js/keyboard_js.c"
                    "modules/loaders/js/ir_js.c"
                    "modules/loaders/js/notification_js.c"
                    "modules/loaders/js/js_loader_app.c"
                    "modules/loaders/js/runtime_js.c"
                    "modules/loaders/js/serial_js.c"
                     "modules/loaders/js/wifi_js.c"
                    "modules/shell/shell_app.c"
                    "modules/shell/shell_parser.c"
                    "modules/shell/shell_executor.c"
                    "modules/shell/shell_builtins.c"
                    ${BRUCE_SELFTEST_SOURCES}
                    "modules/tcp/tcp_app.c"
                     "modules/utils/launcher/launcher_app.c"
                     "modules/utils/help/help_app.c"
                     "modules/utils/serial_commands/serial_commands_app.c"
                     "modules/utils/terminal/terminal_app.c"
                     "modules/utils/process/process_app.c"
                    "modules/utils/notification/notification_app.c"
                    "modules/ir/ir_app.c"
                    "modules/nrf24/nrf24_app.c"
                    "modules/wifi/wifi_app.c"
                    "modules/webui/webui_app.c"
                    REQUIRES
                    args
                    cjson
                    bt
                    esp_event
                    driver
                    esp_adc
                    esp_driver_gpio
                    esp_driver_i2c
                    esp_driver_ledc
                    esp_driver_rmt
                    esp_driver_spi
                    esp_driver_usb_serial_jtag
                    esp_driver_sdspi
                    esp_http_client
                    esp_http_server
                    esp_lcd
                    esp_timer
                    esp_jpeg
                    esp_hid
                     esp_netif
                     esp_partition
                     esp_wifi
                    fatfs
                    log
                    lwip
                     nvs_flash
                     spi_flash
                     littlefs
                     libpng
                     libssh2_esp
                    elf_loader
                    mquickjs
                    INCLUDE_DIRS
                    "."
                     PRIV_INCLUDE_DIRS
                     ".")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BRUCE_WEBUI_ASSET_DIR "${CMAKE_CURRENT_LIST_DIR}/../embedded_resources/web_interface")
set(BRUCE_WEBUI_HEADER "${CMAKE_CURRENT_LIST_DIR}/modules/webui/webfiles.h")
set(BRUCE_WEBUI_ASSETS
    "${BRUCE_WEBUI_ASSET_DIR}/index.html"
    "${BRUCE_WEBUI_ASSET_DIR}/login.html"
    "${BRUCE_WEBUI_ASSET_DIR}/index.css"
    "${BRUCE_WEBUI_ASSET_DIR}/index.js")
add_custom_command(
    OUTPUT "${BRUCE_WEBUI_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_webfiles.py"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_webfiles.py" ${BRUCE_WEBUI_ASSETS}
    COMMENT "Generating compressed WebUI assets"
    VERBATIM)
add_custom_target(bruce_webui_assets DEPENDS "${BRUCE_WEBUI_HEADER}")
add_dependencies(${COMPONENT_LIB} bruce_webui_assets)

set_source_files_properties(${BRUCE_SELFTEST_SOURCES} PROPERTIES
                            COMPILE_OPTIONS
                            "-include;${CMAKE_CURRENT_LIST_DIR}/modules/selftest/selftest_output.h")

# These compile the real built-in module sources against the exported
# core_sdk/ include namespace.  Built-in source must include only
# "core_sdk/..." headers; Core-private includes are rejected by source review.
add_library(bruce_sdk_builtin_launcher_check OBJECT
            "modules/bruce_launcher/bruce_launcher_app.c"
            "modules/bruce_launcher/bruce_launcher_icons.c"
            "modules/bruce_launcher/bruce_launcher_menu.c")
target_include_directories(bruce_sdk_builtin_launcher_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bruce_launcher"
                           "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__cjson/cJSON")
# The launcher module legitimately uses the third-party espressif/cjson
# component API; the core_sdk-only check is about rejecting Core-private headers,
# not component dependencies.

add_library(bruce_sdk_builtin_bnu_check OBJECT "modules/bnu/bnu_app.c")
target_include_directories(bruce_sdk_builtin_bnu_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bnu")

add_library(bruce_sdk_builtin_apps_check OBJECT "modules/apps/apps_app.c")
target_include_directories(bruce_sdk_builtin_apps_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/apps")

add_library(bruce_sdk_builtin_clock_check OBJECT "modules/clock/clock_app.c")
target_include_directories(bruce_sdk_builtin_clock_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/clock")

add_library(bruce_sdk_builtin_config_check OBJECT "modules/config/config_app.c")
target_include_directories(bruce_sdk_builtin_config_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/config")

add_library(bruce_sdk_builtin_wifi_check OBJECT "modules/wifi/wifi_app.c")
target_include_directories(bruce_sdk_builtin_wifi_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/wifi")

add_library(bruce_sdk_builtin_webui_check OBJECT "modules/webui/webui_app.c")
add_dependencies(bruce_sdk_builtin_webui_check bruce_webui_assets)
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

add_library(bruce_sdk_builtin_ir_check OBJECT "modules/ir/ir_app.c")
target_include_directories(bruce_sdk_builtin_ir_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/ir")

add_library(bruce_sdk_builtin_nrf24_check OBJECT "modules/nrf24/nrf24_app.c")
target_include_directories(bruce_sdk_builtin_nrf24_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/nrf24")

add_library(bruce_sdk_builtin_filemanager_check OBJECT "modules/filemanager/filemanager_app.c")
target_include_directories(bruce_sdk_builtin_filemanager_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/filemanager")

add_library(bruce_sdk_builtin_image_loader_check OBJECT "modules/loaders/image/image_loader_app.c")
target_include_directories(bruce_sdk_builtin_image_loader_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/loaders/image")

add_library(bruce_sdk_builtin_utils_launcher_check OBJECT "modules/utils/launcher/launcher_app.c")
target_include_directories(bruce_sdk_builtin_utils_launcher_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/launcher")

add_library(bruce_sdk_builtin_utils_help_check OBJECT "modules/utils/help/help_app.c")
target_include_directories(bruce_sdk_builtin_utils_help_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/help")

add_library(bruce_sdk_builtin_utils_terminal_check OBJECT "modules/utils/terminal/terminal_app.c")
target_include_directories(bruce_sdk_builtin_utils_terminal_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/terminal")

add_library(bruce_sdk_builtin_utils_process_check OBJECT
            "modules/utils/process/process_app.c")
target_include_directories(bruce_sdk_builtin_utils_process_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/process")

add_library(bruce_sdk_builtin_utils_serial_commands_check OBJECT
            "modules/utils/serial_commands/serial_commands_app.c")
target_include_directories(bruce_sdk_builtin_utils_serial_commands_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/serial_commands")

add_library(bruce_sdk_builtin_shell_check OBJECT
            "modules/shell/shell_app.c"
            "modules/shell/shell_parser.c"
            "modules/shell/shell_executor.c"
            "modules/shell/shell_builtins.c")
target_include_directories(bruce_sdk_builtin_shell_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/shell")

add_library(bruce_sdk_builtin_notification_check OBJECT "modules/utils/notification/notification_app.c")
target_include_directories(bruce_sdk_builtin_notification_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/notification")

add_library(bruce_sdk_builtin_tcp_check OBJECT "modules/tcp/tcp_app.c")
target_include_directories(bruce_sdk_builtin_tcp_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/tcp")

add_library(bruce_sdk_builtin_elf_loader_check OBJECT
            "modules/loaders/elf/elf_loader_app.c"
            "modules/loaders/elf/elf_loader_sdk_symbols.c")
target_include_directories(bruce_sdk_builtin_elf_loader_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/loaders/elf"
                           "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__elf_loader/include")
# The ELF loader module legitimately uses the third-party espressif/elf_loader
# component API; the core_sdk-only check is about rejecting Core-private headers,
# not component dependencies.
target_link_libraries(bruce_sdk_builtin_elf_loader_check PRIVATE elf_loader)

add_library(bruce_sdk_builtin_js_loader_check OBJECT
            "modules/loaders/js/dialog_js.c"
            "modules/loaders/js/display_js.c"
            "modules/loaders/js/icon_js.c"
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

set(BRUCE_ARGS_SDK_CHECK_TARGETS
    bruce_sdk_builtin_apps_check
    bruce_sdk_builtin_bnu_check
    bruce_sdk_builtin_clock_check
    bruce_sdk_builtin_config_check
    bruce_sdk_builtin_wifi_check
    bruce_sdk_builtin_webui_check
    bruce_sdk_builtin_bluetooth_check
    bruce_sdk_builtin_bluetooth_hid_check
    bruce_sdk_builtin_ir_check
    bruce_sdk_builtin_nrf24_check
    bruce_sdk_builtin_image_loader_check
    bruce_sdk_builtin_utils_help_check
    bruce_sdk_builtin_utils_process_check
    bruce_sdk_builtin_utils_terminal_check
    bruce_sdk_builtin_shell_check
    bruce_sdk_builtin_notification_check
    bruce_sdk_builtin_tcp_check
    bruce_sdk_builtin_elf_loader_check
    bruce_sdk_builtin_js_loader_check)
foreach(target IN LISTS BRUCE_ARGS_SDK_CHECK_TARGETS)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../components/args")
endforeach()

# modules/selftest is intentionally NOT added to the core_sdk-only compile
# check above: it is the one built-in explicitly permitted to include
# Core-private headers and call FreeRTOS/ESP-IDF directly (see selftest.h
# and migration_plan.md) so it can validate Core's private internals.
