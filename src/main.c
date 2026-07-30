#include <stdio.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/display.h"

#include "core/app_runner/app_runner.h"
#include "core/config/config.h"
#include "core/input/input.h"
#include "core/storage/storage.h"
#include "core/stdio/stdio.h"
#include "core/process/process.h"
#include "freertos/idf_additions.h"

#define MAIN_LAUNCHER_CHECK_INTERVAL_MS 1000

static void main__launch_launcher(void) {
    int result = app_runner__run("launcher", "--gui", true);
    if (result < 0) { printf("Launcher failed to start with code %d\n", result); }
}

bool init_user_interface(void) {
    bool display_ok = display__init() == BRUCE_OK;
    if (!display_ok) printf("Display initialization failed; continuing without LCD\n");

    bool input_ok = input__init() == BRUCE_OK;
    if (!input_ok) printf("Input initialization failed; continuing without physical input\n");

    return display_ok && input_ok;
}

void app_main(void) {
    bool storage_ok = storage__init();
    if (!storage_ok) printf("Storage initialization failed\n");
    if (storage_ok && !config__init()) printf("Configuration is unavailable; using in-memory defaults\n");
    if (stdio__init() != BRUCE_OK) printf("USB serial console initialization failed\n");

    bool ui_ok = init_user_interface();

    app_runner__register_defaults();

    app_runner__run("serial_commands", NULL, true);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    printf("SELFTEST READY\n");
    fflush(stdout);
    return;
#endif

    if (!ui_ok) return;

    main__launch_launcher();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_LAUNCHER_CHECK_INTERVAL_MS));
        if (ui_ok && process_registry__foreground_id() == BRUCE_PROCESS_ID_INVALID) main__launch_launcher();
    }
}
