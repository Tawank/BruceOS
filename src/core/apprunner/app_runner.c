#include "app_runner.h"

#include "launcher.h"
#include "wifi_app.h"

#include <stdio.h>
#include <string.h>

#define APP_RUNNER_MAX_APPS 8

typedef int (*app_entry_t)(int argc, char **argv);

typedef struct {
    const char *name;
    app_entry_t entry;
} app_runner_app_t;

static app_runner_app_t s_apps[APP_RUNNER_MAX_APPS];
static int s_app_count;

static void app_runner__register(const char *name, app_entry_t entry)
{
    if (s_app_count >= APP_RUNNER_MAX_APPS) {
        return;
    }

    s_apps[s_app_count].name = name;
    s_apps[s_app_count].entry = entry;
    s_app_count++;
}

void app_runner__register_defaults(void)
{
    if (s_app_count != 0) {
        return;
    }

    app_runner__register("launcher", launcher_app);
    app_runner__register("wifi", wifi_app);
}

int app_runner__run(const char *app_name, bool in_background)
{
    char *argv[] = {NULL};
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].name, app_name) == 0) {
            return s_apps[i].entry(0, argv);
        }
    }

    printf("Unknown app: %s\n", app_name);
    return -1;
}
