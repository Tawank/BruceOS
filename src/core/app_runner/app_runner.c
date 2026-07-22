#include "app_runner.h"

#include "core/app_runner/app_runner.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/result.h"
#include "modules/bruce_launcher/bruce_launcher.h"
#include "modules/selftest/selftest.h"
#include "modules/wifi/wifi_app.h"

#include <stdio.h>
#include <string.h>

#define APP_RUNNER_MAX_APPS 8

typedef struct {
    const char *name;
    bruce_app_entry_t entry;
} app_runner_app_t;

static app_runner_app_t s_apps[APP_RUNNER_MAX_APPS];
static int s_app_count;

bruce_result_t app_runner__register(const char *name, bruce_app_entry_t entry)
{
    if (name == NULL || name[0] == '\0' || entry == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].name, name) == 0) {
            return BRUCE_ERR_ALREADY_EXISTS;
        }
    }

    if (s_app_count >= APP_RUNNER_MAX_APPS) {
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    s_apps[s_app_count].name = name;
    s_apps[s_app_count].entry = entry;
    s_app_count++;
    return BRUCE_OK;
}

void app_runner__register_defaults(void)
{
    if (s_app_count != 0) {
        return;
    }

    (void)app_runner__register("launcher", bruce_launcher_app);
    (void)app_runner__register("wifi", wifi_app);
    (void)app_runner__register("selftest", selftest_app);
}

int app_runner__run(const char *app_name, const char *arg, bool in_background)
{
    (void)in_background;
    if (app_name == NULL || app_name[0] == '\0') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char *argv[] = {(char *)arg};
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].name, app_name) == 0) {
            return s_apps[i].entry(1, argv);
        }
    }

    printf("Unknown app: %s\n", app_name);
    return BRUCE_ERR_NOT_FOUND;
}
