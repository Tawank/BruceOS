#include "shell_executor.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "shell_builtins.h"

static const char *shell_executor__lookup(void *context, const char *name) {
    return shell_builtins__get((const shell_state_t *)context, name);
}

static bool shell_executor__append_arg(char *out, size_t capacity, size_t *used, const char *arg) {
    size_t needed = *used > 0 ? 1u : 0u;
    needed += 2;
    for (const char *p = arg; *p != '\0'; ++p) needed += (*p == '\\' || *p == '"') ? 2u : 1u;
    if (*used + needed >= capacity) return false;
    if (*used > 0) out[(*used)++] = ' ';
    out[(*used)++] = '"';
    for (const char *p = arg; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '"') out[(*used)++] = '\\';
        out[(*used)++] = *p;
    }
    out[(*used)++] = '"';
    out[*used] = '\0';
    return true;
}

static int shell_executor__wait(bruce_process_id_t child) {
    bruce_process_status_t status;
    for (;;) {
        bruce_result_t waited = process__wait_status(child, 100, &status);
        if (waited == BRUCE_OK) break;
        if (waited == BRUCE_ERR_TIMEOUT) continue;
        if (waited == BRUCE_ERR_CANCELLED) {
            bruce_process_signal_t signal = process__current_signal();
            if (signal != BRUCE_PROCESS_SIGNAL_INT && signal != BRUCE_PROCESS_SIGNAL_TERM) {
                signal = BRUCE_PROCESS_SIGNAL_TERM;
            }
            (void)process__signal(child, signal);
            if (process__wait_status(child, 500, &status) != BRUCE_OK) {
                (void)process__kill(child);
                (void)process__wait_status(child, 500, &status);
            }
            return 128 + (int)signal;
        }
        return 1;
    }
    if (status.reason == BRUCE_PROCESS_TERMINATED || status.reason == BRUCE_PROCESS_KILLED) {
        return 128 + (int)status.signal;
    }
    if (status.exit_code < 0) return 1;
    return status.exit_code & 0xff;
}

static int shell_executor__external(int argc, char **argv) {
    char arguments[SHELL__LINE_MAX * 2];
    size_t used = 0;
    arguments[0] = '\0';
    for (int i = 1; i < argc; ++i) {
        if (!shell_executor__append_arg(arguments, sizeof(arguments), &used, argv[i])) {
            stdio__printf("shell: arguments too long\n");
            return 2;
        }
    }
    int launched;
    if (argv[0][0] == '/' || strncmp(argv[0], "./", 2) == 0) {
        launched = app_runner__run_path(argv[0], used > 0 ? arguments : NULL, true);
    } else {
        launched = app_runner__run(argv[0], used > 0 ? arguments : NULL, true);
    }
    if (launched == BRUCE_ERR_NOT_FOUND || launched == BRUCE_ERR_INVALID_PATH) {
        stdio__printf("shell: %s: not found\n", argv[0]);
        return 127;
    }
    if (launched <= 0) {
        stdio__printf("shell: %s: launch failed (%d)\n", argv[0], launched);
        return 1;
    }
    return shell_executor__wait((bruce_process_id_t)launched);
}

static int shell_executor__command(shell_state_t *state, const shell_command_t *command) {
    char(*words)[SHELL__WORD_MAX] =
        memory__calloc(SHELL__MAX_WORDS, sizeof(*words));
    char *argv[SHELL__MAX_WORDS];
    int argc = 0;
    const char *error = NULL;
    int result = 0;
    if (words == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    if (shell_parser__words(
            command, words, &argc, shell_executor__lookup, state, state->last_status, &error
        ) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "parse error");
        result = 2;
        goto done;
    }
    for (int i = 0; i < argc; ++i) argv[i] = words[i];

    int first_command = 0;
    while (first_command < argc) {
        char *equals = strchr(argv[first_command], '=');
        if (equals == NULL) break;
        size_t name_length = (size_t)(equals - argv[first_command]);
        if (!shell_parser__valid_name(argv[first_command], name_length)) {
            stdio__printf("shell: invalid variable name\n");
            result = 2;
            goto done;
        }
        char name[SHELL__VARIABLE_NAME_MAX];
        if (name_length >= sizeof(name)) {
            stdio__printf("shell: variable name too long\n");
            result = 2;
            goto done;
        }
        memcpy(name, argv[first_command], name_length);
        name[name_length] = '\0';
        int assigned = shell_builtins__set(state, name, equals + 1);
        if (assigned != 0) {
            result = assigned;
            goto done;
        }
        first_command++;
    }
    if (first_command == argc) goto done;
    argc -= first_command;
    argv[0] = words[first_command];
    for (int i = 1; i < argc; ++i) argv[i] = words[first_command + i];
    result = shell_builtins__is_builtin(argv[0]) ? shell_builtins__run(state, argc, argv)
                                                 : shell_executor__external(argc, argv);
done:
    memory__free(words);
    return result;
}

int shell_executor__plan(shell_state_t *state, const shell_plan_t *plan) {
    int status = state->last_status;
    for (size_t i = 0; i < plan->count && !state->exit_requested; ++i) {
        shell_connector_t connector = plan->commands[i].connector;
        bool run = connector == SHELL_CONNECT_NONE || connector == SHELL_CONNECT_SEQUENCE ||
                   (connector == SHELL_CONNECT_AND && status == 0) ||
                   (connector == SHELL_CONNECT_OR && status != 0);
        if (!run) continue;
        status = shell_executor__command(state, &plan->commands[i]);
        state->last_status = status;
    }
    return status;
}
