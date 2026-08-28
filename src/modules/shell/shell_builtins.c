#include "shell_builtins.h"

#include <ctype.h>
#include <errno.h> // IWYU pragma: export
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/environment.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "shell_condition.h"
#include "shell_parser.h"
#include "shell_executor.h"

static const char *const s_shell_builtin_names[] = {
    "echo", "true",  "false", "cd",   "set",  "unset", "export", "clear",
    "reset", "help",  "exit",  "test", "[",    "[[",   "break",  "read",  "time",
};

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
        if (state->variables[index].exported && environment__set(name, value) != BRUCE_OK) {
            memory__free(value_copy);
            return 1;
        }
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
    state->variables[state->variable_count].exported = false;
    state->variable_count++;
    return 0;
}

int shell_builtins__export(shell_state_t *state, const char *name) {
    int index = shell_builtins__find_index(state, name);
    if (index < 0) {
        int status = shell_builtins__set(state, name, "");
        if (status != 0) return status;
        index = shell_builtins__find_index(state, name);
    }
    bruce_result_t result = environment__set(name, state->variables[index].value);
    if (result != BRUCE_OK) {
        stdio__printf("shell: could not export %s (%d)\n", name, result);
        return 1;
    }
    state->variables[index].exported = true;
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

bool shell_builtins__resolve_path(const shell_state_t *state, const char *path, char *out_path) {
    char combined[BRUCE_STORAGE_PATH_MAX * 2];
    const char *working_directory = shell_builtins__get(state, "PWD");
    if (working_directory == NULL || working_directory[0] != '/') working_directory = "/";
    if (path == NULL || path[0] == '\0') path = working_directory;
    int written = path[0] == '/' ? snprintf(combined, sizeof(combined), "%s", path)
                                 : snprintf(
                                       combined,
                                       sizeof(combined),
                                       "%s%s%s",
                                       working_directory,
                                       strcmp(working_directory, "/") == 0 ? "" : "/",
                                       path
                                   );
    if (written < 0 || (size_t)written >= sizeof(combined)) return false;

    size_t out_length = 1;
    out_path[0] = '/';
    out_path[1] = '\0';
    const char *cursor = combined;
    while (*cursor != '\0') {
        while (*cursor == '/') cursor++;
        const char *component = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - component);
        if (length == 0 || (length == 1 && component[0] == '.')) continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            while (out_length > 1 && out_path[out_length - 1] != '/') out_length--;
            if (out_length > 1) out_length--;
            out_path[out_length] = '\0';
            continue;
        }
        size_t separator = out_length > 1 ? 1u : 0u;
        if (out_length + separator + length >= BRUCE_STORAGE_PATH_MAX) return false;
        if (separator != 0) out_path[out_length++] = '/';
        memcpy(out_path + out_length, component, length);
        out_length += length;
        out_path[out_length] = '\0';
    }
    return true;
}

static int shell_builtins__cd(shell_state_t *state, int argc, char **argv) {
    if (argc > 2) {
        stdio__printf("shell: cd: too many arguments\n");
        return 2;
    }
    char path[BRUCE_STORAGE_PATH_MAX];
    if (!shell_builtins__resolve_path(state, argc == 2 ? argv[1] : "/", path)) {
        stdio__printf("cd: invalid path\n");
        return 1;
    }
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) {
        stdio__printf("cd: %s: error %d\n", path, result);
        return 1;
    }
    int status = shell_builtins__set(state, "PWD", path);
    return status == 0 ? shell_builtins__export(state, "PWD") : status;
}

/* Reads one line from stdin (echoing as it's typed, same as a real
 * terminal's cooked-mode echo -- `read` itself never echoes) and splits it
 * on whitespace into argv[1..argc)'s variables, the last of which gets
 * whatever's left of the line (not just its first word), matching bash's
 * own `read` field-splitting. With no variable names at all, the whole
 * (untrimmed) line goes to $REPLY, also matching bash. Returns 1 on EOF/
 * read error (no variables are touched), 2 on a bad variable name, else the
 * exit status of the assignment(s). */
static int shell_builtins__read(shell_state_t *state, int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!shell_parser__valid_name(argv[i], strlen(argv[i]))) {
            stdio__printf("shell: read: %s: invalid variable name\n", argv[i]);
            return 2;
        }
    }
    char line[SHELL__LINE_MAX];
    int length = stdio__read_line(line, sizeof(line), false);
    if (length < 0) return 1;

    if (argc == 1) return shell_builtins__set(state, "REPLY", line);

    char *cursor = line;
    for (int i = 1; i < argc; ++i) {
        while (*cursor != '\0' && isspace((unsigned char)*cursor)) cursor++;
        bool last = i == argc - 1;
        char *value_start = cursor;
        char *value_end;
        if (last) {
            value_end = cursor + strlen(cursor);
            while (value_end > value_start && isspace((unsigned char)value_end[-1])) value_end--;
        } else {
            value_end = cursor;
            while (*value_end != '\0' && !isspace((unsigned char)*value_end)) value_end++;
        }
        char saved = *value_end;
        *value_end = '\0';
        int status = shell_builtins__set(state, argv[i], value_start);
        *value_end = saved;
        if (status != 0) return status;
        cursor = value_end;
    }
    return 0;
}

bool shell_builtins__is_builtin(const char *name) {
    for (size_t i = 0; i < sizeof(s_shell_builtin_names) / sizeof(s_shell_builtin_names[0]); ++i) {
        if (strcmp(name, s_shell_builtin_names[i]) == 0) return true;
    }
    return false;
}

size_t shell_builtins__count(void) {
    return sizeof(s_shell_builtin_names) / sizeof(s_shell_builtin_names[0]);
}

const char *shell_builtins__name(size_t index) {
    return index < shell_builtins__count() ? s_shell_builtin_names[index] : NULL;
}

int shell_builtins__run(shell_state_t *state, int argc, char **argv) {
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; ++i) stdio__printf("%s%s", i > 1 ? " " : "", argv[i]);
        stdio__printf("\n");
        return 0;
    }
    if (strcmp(argv[0], "true") == 0) return 0;
    if (strcmp(argv[0], "false") == 0) return 1;
    if (strcmp(argv[0], "test") == 0 || strcmp(argv[0], "[") == 0 || strcmp(argv[0], "[[") == 0) {
        return shell_condition__run(argc, argv);
    }
    if (strcmp(argv[0], "cd") == 0) return shell_builtins__cd(state, argc, argv);
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
            (void)environment__unset(argv[arg]);
        }
        return 0;
    }
    if (strcmp(argv[0], "export") == 0) {
        if (argc < 2) {
            stdio__printf("shell: export requires NAME=value\n");
            return 2;
        }
        for (int i = 1; i < argc; ++i) {
            char *equals = strchr(argv[i], '=');
            int status = equals != NULL ? shell_builtins__assignment(state, argv[i]) : 0;
            const char *name = argv[i];
            char name_buffer[SHELL__VARIABLE_NAME_MAX];
            if (equals != NULL) {
                size_t length = (size_t)(equals - argv[i]);
                if (length >= sizeof(name_buffer)) return 2;
                memcpy(name_buffer, argv[i], length);
                name_buffer[length] = '\0';
                name = name_buffer;
            } else if (!shell_parser__valid_name(name, strlen(name))) {
                stdio__printf("shell: invalid variable name\n");
                return 2;
            }
            if (status == 0) status = shell_builtins__export(state, name);
            if (status != 0) return status;
        }
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        if (argc != 1) {
            stdio__printf("shell: help takes no arguments; use man <command>\n");
            return 2;
        }
        return shell_executor__page_help();
    }
    if (strcmp(argv[0], "clear") == 0) {
        if (argc != 1) {
            stdio__printf("shell: clear takes no arguments\n");
            return 2;
        }
        /* Erase the screen and home the cursor, matching a real terminal's
         * `clear` -- erasing alone leaves the cursor wherever the last
         * program left it, so the next prompt would draw mid-screen under a
         * blank top half instead of at row 0. */
        (void)stdio__write("\033[2J\033[H", 7);
        return 0;
    }
    if (strcmp(argv[0], "reset") == 0) {
        if (argc != 1) {
            stdio__printf("shell: reset takes no arguments\n");
            return 2;
        }
        /* Full terminal reset (RIS): unlike `clear`, this also drops any
         * SGR attributes/colors a foreground program left set (e.g. a TUI
         * app killed mid-render before it could restore the terminal) and
         * leaves the alternate screen if it's still active -- the on-device
         * equivalent of a real terminal's `reset` command. */
        (void)stdio__write("\033c", 2);
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
    if (strcmp(argv[0], "break") == 0) {
        if (argc > 2) {
            stdio__printf("shell: break: too many arguments\n");
            return 2;
        }
        long levels = 1;
        if (argc == 2) {
            char *end = NULL;
            errno = 0;
            levels = strtol(argv[1], &end, 10);
            if (errno != 0 || end == argv[1] || *end != '\0' || levels < 1 || levels > INT_MAX) {
                stdio__printf("shell: break: numeric argument required\n");
                return 2;
            }
        }
        /* Only meaningful inside a for/while loop -- shell_compound__run()
         * catches (and warns about) a break_requested that outlives every
         * loop it could apply to. */
        state->break_requested = (int)levels;
        return 0;
    }
    if (strcmp(argv[0], "read") == 0) return shell_builtins__read(state, argc, argv);
    /* "time" is intercepted in shell_executor__dispatch() before it ever
     * reaches here (it needs to wrap the function/builtin/external dispatch
     * itself), so it's listed for documentation purposes only -- this branch
     * is otherwise unreachable for it. */
    stdio__printf(
        "Builtins: echo true false cd set unset export clear reset help exit test [ [[ break read time\n"
    );
    stdio__printf("Operators: ; && || and producer | text. Redirection is unsupported.\n");
    stdio__printf("Compound: if/elif/else/fi, for, while, ((...)) arithmetic, functions.\n");
    return 0;
}
