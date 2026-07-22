#include "launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"

#define LAUNCHER__FALLBACK_APP "bruce_launcher"

int launcher_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char *configured = config__get_launcher_app();
    const char *target = (configured != NULL && configured[0] != '\0') ? configured : LAUNCHER__FALLBACK_APP;

    int result = app_runner__run(target, "", false);
    if (result < 0 && strcmp(target, LAUNCHER__FALLBACK_APP) != 0) {
        printf("launcherApp \"%s\" failed to start (%d); falling back to " LAUNCHER__FALLBACK_APP "\n", target,
               result);
        result = app_runner__run(LAUNCHER__FALLBACK_APP, "", false);
    }

    free(configured);
    return result < 0 ? result : 0;
}
