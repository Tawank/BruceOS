#include <stdio.h>

#include "core_sdk/app_runner.h"

#include "core/app_runner/app_runner.h"
#include "core/config/config.h"

void app_main(void)
{
    if (!config__init()) {
        printf("Configuration storage is unavailable; using in-memory defaults\n");
    }
    app_runner__register_defaults();

    int result = app_runner__run("launcher", "", false);

    if (result < 0) {
        printf("Launcher failed to start with code %d\n", result);
    }
}
