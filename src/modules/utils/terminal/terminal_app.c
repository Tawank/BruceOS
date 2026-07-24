#include "terminal_app.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

#define TERMINAL_LINE_MAX 256

/* Strips leading whitespace and copies the first whitespace-delimited token
 * into `token`.  Returns a pointer to the first non-whitespace character
 * following the token, or to the terminating NUL if there is no remainder. */
static const char *terminal__split_line(const char *line, char *token, size_t token_size)
{
    const char *p = line;
    while (isspace((unsigned char)*p)) {
        p++;
    }

    size_t i = 0;
    while (*p != '\0' && !isspace((unsigned char)*p) && i + 1 < token_size) {
        token[i++] = *p++;
    }
    token[i] = '\0';

    while (isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

int terminal__run_line(const char *line)
{
    if (line == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char token[TERMINAL_LINE_MAX];
    const char *rest = terminal__split_line(line, token, sizeof(token));

    if (token[0] == '\0') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    const char *arg = (rest[0] != '\0') ? rest : NULL;

    if (token[0] == '/' || strncmp(token, "./", 2) == 0) {
        return app_runner__run_path(token, arg, false);
    }

    return app_runner__run(token, arg, false);
}

int terminal_app_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char line[TERMINAL_LINE_MAX];

    for (;;) {
        printf("bruce> ");
        fflush(stdout);

        int len = bruce_stdio_read_line(line, sizeof(line), false);
        if (len < 0) {
            break;
        }
        if (line[0] == '\0') {
            continue;
        }

        if (strcmp(line, "exit") == 0) {
            return 0;
        }

        int result = terminal__run_line(line);
        if (result > 0) {
            printf("started task %u\n", (unsigned int)result);
        } else {
            printf("error %d\n", result);
        }
    }

    return 0;
}
