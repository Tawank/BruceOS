#include "shell_app.h"

#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "shell_executor.h"
#include "shell_parser.h"

void shell__state_init(shell_state_t *state) { memset(state, 0, sizeof(*state)); }

int shell__execute_line(shell_state_t *state, const char *line) {
    if (state == NULL || line == NULL) return 2;
    shell_plan_t plan;
    const char *error = NULL;
    if (shell_parser__plan(line, &plan, &error) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "syntax error");
        state->last_status = 2;
        return 2;
    }
    if (plan.count == 0) return 0;
    return shell_executor__plan(state, &plan);
}

static int shell__run_script(shell_state_t *state, const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t opened = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (opened != BRUCE_OK) {
        stdio__printf("shell: %s: cannot open (%d)\n", path, opened);
        return 1;
    }
    char line[SHELL__LINE_MAX];
    size_t used = 0;
    int status = state->last_status;
    bool overlong = false;
    for (;;) {
        char chunk[128];
        size_t size = 0;
        bruce_result_t read = storage__read(file, chunk, sizeof(chunk), &size);
        if (read != BRUCE_OK) {
            stdio__printf("shell: %s: read error (%d)\n", path, read);
            status = 1;
            break;
        }
        for (size_t i = 0; i < size; ++i) {
            char c = chunk[i];
            if (c == '\n') {
                if (overlong) {
                    stdio__printf("shell: %s: script line too long\n", path);
                    status = 2;
                    goto done;
                }
                if (used > 0 && line[used - 1] == '\r') used--;
                line[used] = '\0';
                status = shell__execute_line(state, line);
                used = 0;
                if (state->exit_requested) goto done;
            } else if (used + 1 < sizeof(line)) {
                line[used++] = c;
            } else {
                overlong = true;
            }
        }
        if (size == 0) break;
    }
    if (overlong) {
        stdio__printf("shell: %s: script line too long\n", path);
        status = 2;
    } else if (used > 0 && !state->exit_requested) {
        if (line[used - 1] == '\r') used--;
        line[used] = '\0';
        status = shell__execute_line(state, line);
    }
done:
    (void)storage__close(file);
    return state->exit_requested ? state->exit_status : status;
}

static int shell__interactive(shell_state_t *state, bool suppress_echo) {
    char line[SHELL__LINE_MAX];
    while (!state->exit_requested) {
        stdio__printf("bruce$ ");
        int length = bruce_stdio_read_line(line, sizeof(line), suppress_echo);
        if (length == BRUCE_ERR_CANCELLED) return 128 + (int)process__current_signal();
        if (length < 0) break;
        (void)shell__execute_line(state, line);
    }
    return state->exit_requested ? state->exit_status : state->last_status;
}

int shell_app_main(int argc, char **argv) {
    shell_state_t *state = memory__calloc(1, sizeof(*state));
    if (state == NULL) return BRUCE_ERR_NO_MEMORY;
    shell__state_init(state);
    int status;
    if (argc <= 1 || (argc == 2 && strcmp(argv[1], "-i") == 0)) {
        status = shell__interactive(state, false);
        goto done;
    }
    if (argc == 3 && strcmp(argv[1], "-i") == 0 && strcmp(argv[2], "--no-echo") == 0) {
        status = shell__interactive(state, true);
        goto done;
    }
    if (strcmp(argv[1], "-c") == 0) {
        if (argc != 3) {
            stdio__printf("shell: -c requires one command string\n");
            status = 2;
            goto done;
        }
        status = shell__execute_line(state, argv[2]);
        if (state->exit_requested) status = state->exit_status;
        goto done;
    }
    if (argc != 2 || argv[1][0] != '/' || strlen(argv[1]) < 4 ||
        strcmp(argv[1] + strlen(argv[1]) - 3, ".sh") != 0) {
        stdio__printf("shell: expected -i, -c command, or absolute .sh path\n");
        status = 2;
        goto done;
    }
    status = shell__run_script(state, argv[1]);
done:
    memory__free(state);
    return status;
}

static bool shell__quote_path(const char *path, char *out, size_t capacity) {
    size_t used = 0;
    if (capacity < 3) return false;
    out[used++] = '"';
    for (const char *p = path; *p != '\0'; ++p) {
        if ((*p == '\\' || *p == '"') && used + 1 >= capacity) return false;
        if (*p == '\\' || *p == '"') out[used++] = '\\';
        if (used + 1 >= capacity) return false;
        out[used++] = *p;
    }
    out[used++] = '"';
    out[used] = '\0';
    return true;
}

int shell_loader__run_path(const char *path, const char *arg, bool in_background) {
    if (path == NULL) return BRUCE_ERR_INVALID_PATH;
    if (arg != NULL && arg[0] != '\0') return BRUCE_ERR_UNSUPPORTED;
    char arguments[BRUCE_STORAGE_PATH_MAX * 2];
    if (!shell__quote_path(path, arguments, sizeof(arguments))) return BRUCE_ERR_INVALID_PATH;
    return app_runner__run("shell", arguments, in_background);
}
