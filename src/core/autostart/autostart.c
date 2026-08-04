#include "autostart.h"

#include <stdio.h>
#include <string.h>

#include "core/config/config.h"
#include "core_sdk/app_runner.h"

static bool autostart__command_is_serial_commands(const char *command) {
    static const char name[] = "serial_commands";
    size_t len = sizeof(name) - 1u;
    return strncmp(command, name, len) == 0 && (command[len] == '\0' || command[len] == ' ');
}

/* True iff the command's leading "key=value" environment tokens include
 * "BG=0", i.e. it overrides the autostart default and actually claims the
 * foreground screen (e.g. "BG=0 bootanimation"). Startup apps that stay in
 * the background (the default here) are headless services like "input" or
 * the launcher supervisor "-s" loop; tagging them GUI=1 too would make
 * process switch next/prev cycle onto a background process that never draws
 * anything. */
static bool autostart__command_requests_foreground(const char *command) {
    const char *cursor = command;
    for (;;) {
        while (*cursor == ' ') cursor++;
        const char *token_end = cursor;
        while (*token_end != '\0' && *token_end != ' ') token_end++;
        size_t token_len = (size_t)(token_end - cursor);
        if (token_len == 0 || memchr(cursor, '=', token_len) == NULL) return false;
        if (token_len == 4 && strncmp(cursor, "BG=0", 4) == 0) return true;
        cursor = token_end;
    }
}

void autostart__run(bool display_ok) {
    const bruce_config_startup_apps_t *apps = config__get_startup_apps();
    if (apps == NULL) return;
    for (size_t i = 0; i < apps->count; ++i) {
        const char *command = apps->items[i];
        char with_gui[CONFIG__STARTUP_APP_MAX_LEN + 8];
        if (display_ok && !autostart__command_is_serial_commands(command) && strncmp(command, "GUI=", 4) != 0 &&
            autostart__command_requests_foreground(command)) {
            snprintf(with_gui, sizeof(with_gui), "GUI=1 %s", command);
            command = with_gui;
        }
        int result = app_runner__run_command(command, BRUCE_LAUNCH_BACKGROUND);
        if (result < 0) printf("Startup app \"%s\" failed with code %d\n", apps->items[i], result);
    }
}
