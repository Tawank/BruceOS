#include "shell_builtins.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/stdio.h"
#include "shell_parser.h"

const char *shell_builtins__get(const shell_state_t *state, const char *name) {
    for (size_t i = 0; i < SHELL__MAX_VARIABLES; ++i) {
        if (state->variables[i].in_use && strcmp(state->variables[i].name, name) == 0) {
            return state->variables[i].value;
        }
    }
    return NULL;
}

int shell_builtins__set(shell_state_t *state, const char *name, const char *value) {
    size_t name_length = strlen(name);
    size_t value_length = strlen(value);
    if (!shell_parser__valid_name(name, name_length) || name_length >= SHELL__VARIABLE_NAME_MAX ||
        value_length >= SHELL__VARIABLE_VALUE_MAX) {
        stdio__printf("shell: invalid or oversized variable assignment\n");
        return 2;
    }
    shell_variable_t *free_slot = NULL;
    for (size_t i = 0; i < SHELL__MAX_VARIABLES; ++i) {
        if (state->variables[i].in_use && strcmp(state->variables[i].name, name) == 0) {
            free_slot = &state->variables[i];
            break;
        }
        if (!state->variables[i].in_use && free_slot == NULL) free_slot = &state->variables[i];
    }
    if (free_slot == NULL) {
        stdio__printf("shell: variable limit reached\n");
        return 1;
    }
    free_slot->in_use = true;
    memcpy(free_slot->name, name, name_length + 1);
    memcpy(free_slot->value, value, value_length + 1);
    return 0;
}

static int shell_builtins__assignment(shell_state_t *state, const char *assignment) {
    const char *equals = strchr(assignment, '=');
    if (equals == NULL) return 2;
    size_t name_length = (size_t)(equals - assignment);
    if (name_length >= SHELL__VARIABLE_NAME_MAX || !shell_parser__valid_name(assignment, name_length)) {
        stdio__printf("shell: invalid variable name\n");
        return 2;
    }
    char name[SHELL__VARIABLE_NAME_MAX];
    memcpy(name, assignment, name_length);
    name[name_length] = '\0';
    return shell_builtins__set(state, name, equals + 1);
}

bool shell_builtins__is_builtin(const char *name) {
    static const char *const names[] = {"echo", "true", "false", "set", "unset", "export", "exit", "help"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

int shell_builtins__run(shell_state_t *state, int argc, char **argv) {
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; ++i) stdio__printf("%s%s", i > 1 ? " " : "", argv[i]);
        stdio__printf("\n");
        return 0;
    }
    if (strcmp(argv[0], "true") == 0) return 0;
    if (strcmp(argv[0], "false") == 0) return 1;
    if (strcmp(argv[0], "set") == 0) {
        if (argc != 1) {
            stdio__printf("shell: set takes no arguments\n");
            return 2;
        }
        for (size_t i = 0; i < SHELL__MAX_VARIABLES; ++i) {
            if (state->variables[i].in_use) {
                stdio__printf("%s=%s\n", state->variables[i].name, state->variables[i].value);
            }
        }
        return 0;
    }
    if (strcmp(argv[0], "unset") == 0) {
        if (argc < 2) {
            stdio__printf("shell: unset requires a name\n");
            return 2;
        }
        for (int arg = 1; arg < argc; ++arg) {
            if (!shell_parser__valid_name(argv[arg], strlen(argv[arg]))) {
                stdio__printf("shell: invalid variable name\n");
                return 2;
            }
            for (size_t i = 0; i < SHELL__MAX_VARIABLES; ++i) {
                if (state->variables[i].in_use && strcmp(state->variables[i].name, argv[arg]) == 0) {
                    memset(&state->variables[i], 0, sizeof(state->variables[i]));
                }
            }
        }
        return 0;
    }
    if (strcmp(argv[0], "export") == 0) {
        if (argc < 2) {
            stdio__printf("shell: export requires NAME=value\n");
            return 2;
        }
        for (int i = 1; i < argc; ++i) {
            int status = shell_builtins__assignment(state, argv[i]);
            if (status != 0) return status;
        }
        return 0;
    }
    if (strcmp(argv[0], "exit") == 0) {
        int status = state->last_status;
        if (argc > 2) {
            stdio__printf("shell: exit: too many arguments\n");
            return 2;
        }
        if (argc == 2) {
            char *end = NULL;
            errno = 0;
            long parsed = strtol(argv[1], &end, 10);
            if (errno != 0 || end == argv[1] || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
                stdio__printf("shell: exit: numeric argument required\n");
                status = 2;
            } else {
                status = (int)((unsigned long)parsed & 0xffu);
            }
        }
        state->exit_requested = true;
        state->exit_status = status;
        return status;
    }
    stdio__printf("Builtins: echo true false set unset export exit help\n");
    stdio__printf("Operators: ; && ||. Pipes and redirection are unsupported.\n");
    return 0;
}
