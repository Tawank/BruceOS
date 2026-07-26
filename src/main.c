#include <stdio.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/display.h"

#include "core/app_runner/app_runner.h"
#include "core/config/config.h"
#include "core/input/input.h"
#include "core/ir/ir.h"
#include "freertos/idf_additions.h"
 
void app_main(void)
{
    if (!config__init()) {
        printf("Configuration storage is unavailable; using in-memory defaults\n");
    }
    bool display_ok = display__init() == BRUCE_OK;
    if (!display_ok) {
        printf("Display initialization failed; continuing without LCD\n");
    }
    bool input_ok = input__init() == BRUCE_OK;
    if (!input_ok) {
        printf("Input initialization failed; continuing without physical input\n");
    }
    if (ir__init() != BRUCE_OK) {
        printf("Infrared initialization failed; IR is unavailable\n");
    }
    app_runner__register_defaults();

    const char *launcher_args = (display_ok && input_ok) ? "--gui" : "";
    int result = app_runner__run("launcher", launcher_args, false);

    if (result < 0) {
        printf("Launcher failed to start with code %d\n", result);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
