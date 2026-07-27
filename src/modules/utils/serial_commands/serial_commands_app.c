#include "serial_commands_app.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/task.h"

#define SERIAL_COMMANDS__LINE_MAX 256

static const char *serial_commands__split_line(const char *line, char *token, size_t token_size)
{
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    size_t i = 0;
    while (*p != '\0' && !isspace((unsigned char)*p) && i + 1 < token_size) token[i++] = *p++;
    token[i] = '\0';
    while (isspace((unsigned char)*p)) p++;
    return p;
}

int serial_commands__run_line(const char *line, bool in_background)
{
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

int serial_commands_app_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[SERIAL_COMMANDS__LINE_MAX];
    for (;;) {
        printf("bruce> ");
        fflush(stdout);
        int len = bruce_stdio_read_line(line, sizeof(line), false);
        if (len < 0) break;
        if (line[0] == '\0') continue;
        if (strcmp(line, "exit") == 0) return 0;
        int result = serial_commands__run_line(line, false);
        if (result > 0) {
            printf("started task %u\n", (unsigned int)result);
            while (task__wait((bruce_task_id_t)result, 100) == BRUCE_ERR_TIMEOUT) {
                if (runtime__delay(1) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
            }
        } else {
            printf("error %d\n", result);
        }
    }
    return 0;
}
