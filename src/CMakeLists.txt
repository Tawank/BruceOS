idf_component_register(SRCS
                    "main.c"
                    "core/app_runner/app_runner.c"
                    "core/config/config.c"
                    "core/elf/elf.c"
                    "core/js/js.c"
                    "core/memory/memory.c"
                    "core/storage/storage.c"
                    "core/task/task.c"
                    "core/wifi/wifi_common.c"
                    "modules/bruce_launcher/bruce_launcher.c"
                    "modules/selftest/selftest.c"
                    "modules/utils/launcher/launcher.c"
                    "modules/wifi/wifi_app.c"
                    REQUIRES
                    cjson
                    esp_event
                    driver
                    esp_driver_sdspi
                    esp_netif
                    esp_wifi
                    fatfs
                    log
                    lwip
                    spiffs
                    INCLUDE_DIRS
                    "."
                    PRIV_INCLUDE_DIRS
                    ".")

# These compile the real built-in module sources against the exported
# core_sdk/ include namespace.  Built-in source must include only
# "core_sdk/..." headers; Core-private includes are rejected by source review.
add_library(bruce_sdk_builtin_launcher_check OBJECT "modules/bruce_launcher/bruce_launcher.c")
target_include_directories(bruce_sdk_builtin_launcher_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/bruce_launcher")

add_library(bruce_sdk_builtin_wifi_check OBJECT "modules/wifi/wifi_app.c")
target_include_directories(bruce_sdk_builtin_wifi_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/wifi")

add_library(bruce_sdk_builtin_utils_launcher_check OBJECT "modules/utils/launcher/launcher.c")
target_include_directories(bruce_sdk_builtin_utils_launcher_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/utils/launcher")

# modules/selftest is intentionally NOT added to the core_sdk-only compile
# check above: it is the one built-in explicitly permitted to include
# Core-private headers and call FreeRTOS/ESP-IDF directly (see selftest.h
# and migration_BruceIDF.md) so it can validate Core's private internals.
