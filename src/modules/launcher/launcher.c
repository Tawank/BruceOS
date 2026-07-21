#include "launcher.h"

#include <stdio.h>

#include "core/apprunner/app_runner.h"

int launcher_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("Launcher started\n");
    printf("Launcher menu: wifi\n");

    char *wifi_argv[] = {"webui"};
    int result = app_runner__run("wifi", 1, wifi_argv);

    if (result != 0) {
        printf("Wifi app returned %d\n", result);
    }

    return 0;
}
