#include "bruce_launcher.h"

#include <stdio.h>

#include "core_sdk/app_runner.h"

int bruce_launcher_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("Launcher started\n");
    printf("Launcher menu: wifi\n");

    int result = app_runner__run("wifi", "--gui", false);

    if (result != 0) {
        printf("Wifi app returned %d\n", result);
    }

    return 0;
}
