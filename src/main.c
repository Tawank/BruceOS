#include <stdio.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/display.h"

#include "core/app_runner/app_runner.h"
#include "core/config/config.h"
#include "core/input/input.h"
#include "core/stdio/stdio.h"
#include "freertos/idf_additions.h"

bool init_user_interface(void) {
    bool display_ok = display__init() == BRUCE_OK;
    if (!display_ok) printf("Display initialization failed; continuing without LCD\n");

    bool input_ok = input__init() == BRUCE_OK;
    if (!input_ok) printf("Input initialization failed; continuing without physical input\n");

    return display_ok && input_ok;
}

void app_main(void) {
    if (!config__init()) printf("Configuration storage is unavailable; using in-memory defaults\n");
    if (stdio__init() != BRUCE_OK) printf("USB serial console initialization failed\n");

    bool ui_ok = init_user_interface();

    app_runner__register_defaults();

    app_runner__run("serial_commands", NULL, true);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    printf("SELFTEST READY\n");
    fflush(stdout);
    return;
#endif

    if (ui_ok) {
        int result = app_runner__run("launcher", "--gui", true);
        if (result < 0) { printf("Launcher failed to start with code %d\n", result); }
    }

    while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
}
