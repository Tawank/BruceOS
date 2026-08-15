#include "shell_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "shell_executor.h"
#include "shell_builtins.h"
#include "shell_console.h"
#include "shell_history.h"
#include "shell_internal.h"
#include "shell_parser.h"

void shell__state_init(shell_state_t *state) {
    memset(state, 0, sizeof(*state));
    size_t count = environment__count();
    for (size_t i = 0; i < count; ++i) {
        const char *name = NULL;
        const char *value = NULL;
        if (environment__get_at(i, &name, &value) != BRUCE_OK ||
            shell_builtins__set(state, name, value) != 0) {
            continue;
        }
        state->variables[state->variable_count - 1].exported = true;
    }
}

void shell__state_free(shell_state_t *state) {
    if (state == NULL) return;
    for (size_t i = 0; i < state->variable_count; ++i) {
        memory__free(state->variables[i].name);
        memory__free(state->variables[i].value);
    }
    memory__free(state->variables);
    state->variables = NULL;
    state->variable_count = 0;
    state->variable_capacity = 0;
}

int shell__execute_line(shell_state_t *state, const char *line) {
    if (state == NULL || line == NULL) return 2;
    shell_plan_t plan = {0};
    const char *error = NULL;
    if (shell_parser__plan(line, &plan, &error) != 0) {
        shell_parser__plan_free(&plan);
        stdio__printf("shell: %s\n", error != NULL ? error : "syntax error");
        state->last_status = 2;
        return 2;
    }
    if (plan.count == 0) {
        shell_parser__plan_free(&plan);
        return 0;
    }
    int status = shell_executor__plan(state, &plan);
    shell_parser__plan_free(&plan);
    return status;
}

static int shell__run_script(shell_state_t *state, const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t opened = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (opened != BRUCE_OK) {
        stdio__printf("shell: %s: cannot open (%d)\n", path, opened);
        return 1;
    }
    char *line = memory__malloc(SHELL__LINE_MAX);
    if (line == NULL) {
        stdio__printf("shell: out of memory\n");
        (void)storage__close(file);
        return 1;
    }
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
            } else if (used + 1 < SHELL__LINE_MAX) {
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
    memory__free(line);
    (void)storage__close(file);
    return state->exit_requested ? state->exit_status : status;
}

static int shell__interactive(shell_state_t *state, bool suppress_echo) {
    char *line = memory__malloc(SHELL__LINE_MAX);
    if (line == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    bool skip_lf = false;
    while (!state->exit_requested) {
        int length;
        if (suppress_echo) {
            length = stdio__read_line(line, SHELL__LINE_MAX, true);
        } else {
            length = shell_console__read_line(line, SHELL__LINE_MAX, &skip_lf);
        }
        if (length == BRUCE_ERR_CANCELLED) {
            int status = 128 + (int)process__current_signal();
            memory__free(line);
            return status;
        }
        if (length < 0) break;
        if (length > 0) (void)shell_history__append(SHELL_HISTORY_PATH, line);
        (void)shell__execute_line(state, line);
    }
    memory__free(line);
    return state->exit_requested ? state->exit_status : state->last_status;
}

static bool shell__is_script_path(const char *path) {
    size_t length = strlen(path);
    return path[0] == '/' && length >= 4 && strcmp(path + length - 3, ".sh") == 0;
}

static char *shell__dup(const char *text) {
    size_t length = strlen(text);
    char *copy = memory__malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

int shell_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Run the interactive shell, a single command, or a script.");
    ap_add_flag(parser, "i");
    ap_set_opt_help(parser, "i", "Run interactively");
    ap_add_flag(parser, "no-echo");
    ap_set_opt_help(parser, "no-echo", "Suppress input echo in interactive mode (with -i)");
    ap_add_str_opt(parser, "c", NULL);
    ap_set_opt_help(parser, "c", "Run a single command string");
    ap_add_optional_arg(parser, "script", "Absolute path to a .sh script to run");

    if (!ap_parse(parser, argc, argv)) {
        ap_status_t parse_status = ap_get_status(parser);
        ap_free(parser);
        return parse_status == AP_STATUS_HELP || parse_status == AP_STATUS_VERSION ? 0 : 2;
    }

    bool interactive_flag = ap_found(parser, "i");
    bool no_echo = ap_found(parser, "no-echo");
    bool has_command = ap_found(parser, "c");
    const char *command_value = ap_get_str_value(parser, "c");
    const char *script_value = ap_get_arg(parser, "script");
    bool has_script = script_value != NULL;

    bool valid = !(no_echo && !interactive_flag) &&
                 !(has_command && (interactive_flag || no_echo || has_script)) &&
                 !(has_script && (interactive_flag || no_echo || has_command)) &&
                 (!has_script || shell__is_script_path(script_value));

    char *command = has_command && valid ? shell__dup(command_value) : NULL;
    char *script = has_script && valid ? shell__dup(script_value) : NULL;
    bool alloc_failed = (has_command && valid && command == NULL) || (has_script && valid && script == NULL);
    ap_free(parser);

    if (!valid || alloc_failed) {
        stdio__printf(
            alloc_failed ? "shell: out of memory\n" : "shell: expected -i, -c command, or absolute .sh path\n"
        );
        memory__free(command);
        memory__free(script);
        return alloc_failed ? 1 : 2;
    }

    shell_state_t *state = memory__calloc(1, sizeof(*state));
    if (state == NULL) {
        memory__free(command);
        memory__free(script);
        return BRUCE_ERR_NO_MEMORY;
    }
    shell__state_init(state);

    int status;
    if (has_command) {
        status = shell__execute_line(state, command);
        if (state->exit_requested) status = state->exit_status;
    } else if (has_script) {
        status = shell__run_script(state, script);
    } else {
        status = shell__interactive(state, no_echo);
    }

    shell__state_free(state);
    memory__free(state);
    memory__free(command);
    memory__free(script);
    return status;
}
