#include "shell_builtins.h"

#include <errno.h> // IWYU pragma: export
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/stdio.h"
#include "shell_parser.h"

static int shell_builtins__find_index(const shell_state_t *state, const char *name) {
    for (size_t i = 0; i < state->variable_count; ++i) {
        if (strcmp(state->variables[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static char *shell_builtins__dup(const char *text, size_t length) {
    char *copy = memory__malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void shell_builtins__remove_at(shell_state_t *state, size_t index) {
    memory__free(state->variables[index].name);
    memory__free(state->variables[index].value);
    size_t last = state->variable_count - 1;
    if (index != last) state->variables[index] = state->variables[last];
    state->variable_count--;
}

const char *shell_builtins__get(const shell_state_t *state, const char *name) {
    int index = shell_builtins__find_index(state, name);
    return index >= 0 ? state->variables[index].value : NULL;
}

int shell_builtins__set(shell_state_t *state, const char *name, const char *value) {
    size_t name_length = strlen(name);
    size_t value_length = strlen(value);
    if (!shell_parser__valid_name(name, name_length) || name_length >= SHELL__VARIABLE_NAME_MAX ||
        value_length >= SHELL__VARIABLE_VALUE_MAX) {
        stdio__printf("shell: invalid or oversized variable assignment\n");
        return 2;
    }
    char *value_copy = shell_builtins__dup(value, value_length);
    if (value_copy == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    int index = shell_builtins__find_index(state, name);
    if (index >= 0) {
        memory__free(state->variables[index].value);
        state->variables[index].value = value_copy;
        return 0;
    }
    if (state->variable_count >= SHELL__MAX_VARIABLES) {
        stdio__printf("shell: variable limit reached\n");
        memory__free(value_copy);
        return 1;
    }
    if (state->variable_count >= state->variable_capacity) {
        size_t new_capacity = state->variable_capacity == 0 ? 4 : state->variable_capacity * 2;
        if (new_capacity > SHELL__MAX_VARIABLES) new_capacity = SHELL__MAX_VARIABLES;
        shell_variable_t *grown = memory__realloc(state->variables, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            stdio__printf("shell: out of memory\n");
            memory__free(value_copy);
            return 1;
        }
        state->variables = grown;
        state->variable_capacity = new_capacity;
    }
    char *name_copy = shell_builtins__dup(name, name_length);
    if (name_copy == NULL) {
        stdio__printf("shell: out of memory\n");
        memory__free(value_copy);
        return 1;
    }
    state->variables[state->variable_count].name = name_copy;
    state->variables[state->variable_count].value = value_copy;
    state->variable_count++;
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
    static const char *const names[] = {"echo", "true", "false", "set", "unset", "export", "clear", "exit"};
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
        for (size_t i = 0; i < state->variable_count; ++i) {
            stdio__printf("%s=%s\n", state->variables[i].name, state->variables[i].value);
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
            int index = shell_builtins__find_index(state, argv[arg]);
            if (index >= 0) shell_builtins__remove_at(state, (size_t)index);
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
    if (strcmp(argv[0], "clear") == 0) {
        if (argc != 1) {
            stdio__printf("shell: clear takes no arguments\n");
            return 2;
        }
        (void)stdio__write("\033[2J", 4);
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
    stdio__printf("Builtins: echo true false set unset export clear exit\n");
    stdio__printf("Operators: ; && || and producer | text. Redirection is unsupported.\n");
    return 0;
}
