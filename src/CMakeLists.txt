idf_component_register(SRCS
                    "main.c"
                    "core/app_runner/app_runner.c"
                    "core/config/config.c"
                    "core/dialog/dialog.c"
                    "core/manifest/manifest.c"
                    "core/memory/memory.c"
                    "core/permission/permission.c"
                    "core/storage/storage.c"
                    "core/task/task.c"
                    "core/wifi/wifi_common.c"
                    "modules/bruce_launcher/bruce_launcher.c"
                    "modules/loaders/elf/elf_loader.c"
                    "modules/loaders/elf/elf_loader_sdk_symbols.c"
                    "modules/selftest/app_runner_test.c"
                    "modules/selftest/config_test.c"
                    "modules/selftest/fake_elf.c"
                    "modules/selftest/loader_test.c"
                    "modules/selftest/permission_test.c"
                    "modules/selftest/selftest.c"
                    "modules/selftest/storage_test.c"
                    "modules/selftest/task_test.c"
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
                    elf_loader
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

add_library(bruce_sdk_builtin_elf_loader_check OBJECT
            "modules/loaders/elf/elf_loader.c"
            "modules/loaders/elf/elf_loader_sdk_symbols.c")
target_include_directories(bruce_sdk_builtin_elf_loader_check PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}"
                           "${CMAKE_CURRENT_LIST_DIR}/modules/loaders/elf"
                           "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__elf_loader/include")
# The ELF loader module legitimately uses the third-party espressif/elf_loader
# component API; the core_sdk-only check is about rejecting Core-private headers,
# not component dependencies.
target_link_libraries(bruce_sdk_builtin_elf_loader_check PRIVATE elf_loader)

# modules/selftest is intentionally NOT added to the core_sdk-only compile
# check above: it is the one built-in explicitly permitted to include
# Core-private headers and call FreeRTOS/ESP-IDF directly (see selftest.h
# and migration_BruceIDF.md) so it can validate Core's private internals.
