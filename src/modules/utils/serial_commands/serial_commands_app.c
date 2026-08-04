#include "serial_commands_app.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "modules/shell/shell_console.h"

#define SERIAL_COMMANDS__LINE_MAX 256
#define SERIAL_COMMANDS__WAIT_INTERVAL_MS 10

int serial_commands__run_line(const char *line, bool in_background) {
    return app_runner__run_command(line, in_background ? BRUCE_LAUNCH_BACKGROUND : BRUCE_LAUNCH_FOREGROUND);
}

int serial_commands_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    shell_console__reset_ready();
    int result = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
    if (result < 0) return result;
    while (process__wait((bruce_process_id_t)result, 100) == BRUCE_ERR_TIMEOUT) {
        if (runtime__delay(10) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    return 0;
}

bool serial_commands__wait_ready(uint32_t timeout_ms) {
    uint64_t started = runtime__now();
    while (!shell_console__is_ready() && runtime__now() - started < timeout_ms) {
        if (runtime__delay(SERIAL_COMMANDS__WAIT_INTERVAL_MS) != BRUCE_OK) return false;
    }
    return shell_console__is_ready();
}
