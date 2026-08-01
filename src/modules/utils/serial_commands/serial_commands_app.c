#include "serial_commands_app.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "modules/shell/shell_console.h"

#define SERIAL_COMMANDS__LINE_MAX 256
#define SERIAL_COMMANDS__WAIT_INTERVAL_MS 1

static const char *serial_commands__split_line(const char *line, char *token, size_t token_size) {
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    size_t i = 0;
    while (*p != '\0' && !isspace((unsigned char)*p) && i + 1 < token_size) token[i++] = *p++;
    token[i] = '\0';
    while (isspace((unsigned char)*p)) p++;
    return p;
}

int serial_commands__run_line(const char *line, bool in_background) {
    if (line == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    char token[SERIAL_COMMANDS__LINE_MAX];
    const char *rest = serial_commands__split_line(line, token, sizeof(token));
    if (token[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    const char *arg = rest[0] != '\0' ? rest : NULL;
    if (token[0] == '/' || strncmp(token, "./", 2) == 0) {
        return app_runner__run_path(token, arg, in_background);
    }
    return app_runner__run(token, arg, in_background);
}

int serial_commands_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell_console__reset_ready();
    int result = app_runner__run("shell", "-i", true);
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
