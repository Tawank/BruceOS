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
#include "shell_compound.h"
#include "shell_condition.h"
#include "shell_parser.h"
#include "shell_executor.h"
#include "shell_jobs.h"

/* Name + one-line description, kept side by side so a new builtin can't add
 * one without the other -- shell_builtins__description() backs both `man`'s
 * "Shell Built-ins" listing and its --gen-md section (see man_app.c). */
typedef struct {
    const char *name;
    const char *description;
} shell_builtin_entry_t;

static const shell_builtin_entry_t s_shell_builtins[] = {
    {"echo", "Print arguments"},
    {"printf", "Print formatted output"},
    {"true", "Return success"},
    {"false", "Return failure"},
    {"cd", "Change the working directory"},
    {"set", "List shell variables"},
    {"unset", "Remove a variable"},
    {"export", "Mark a variable for export to child processes"},
    {"clear", "Erase the screen"},
    {"reset", "Reset the terminal"},
    {"help", "List shell built-ins and usage"},
    {"exit", "Exit the shell"},
    {"test", "Evaluate a conditional expression"},
    {"[", "Evaluate a conditional expression"},
    {"[[", "Evaluate a conditional expression"},
    {"break", "Break out of a for/while loop"},
    {"continue", "Skip to the next iteration of a for/while loop"},
    {"return", "Return from a shell function"},
    {"shift", "Shift positional parameters left"},
    {"read", "Read a line into a variable"},
    {"time", "Time how long a command takes"},
    {"local", "Declare a function-local variable"},
    {"jobs", "List background jobs"},
    {"wait", "Wait for a background job to finish"},
    {"kill", "Send a signal to a job or process"},
    {"fg", "Bring a background job to the foreground"},
    {"bg", "Resume a paused job in the background"},
    {"disown", "Stop tracking a job without touching it"},
    {"trap", "Run a command when a signal is received or the shell exits"},
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

void shell_builtins__unset(shell_state_t *state, const char *name) {
    int index = shell_builtins__find_index(state, name);
    if (index >= 0) shell_builtins__remove_at(state, (size_t)index);
    (void)environment__unset(name);
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

/* Adds `name` to the innermost active function call's `local` frame the
 * first time it's localized during that call, snapshotting whatever value
 * (if any) it currently holds so shell_compound__call_function() can put it
 * back -- or unset `name` entirely, if it didn't exist before -- once the
 * call returns. A second `local name` for the same name within the same
 * call is a no-op here (the call already knows what to restore to); the
 * caller still goes on to perform the actual assignment either way, same
 * as bash re-localizing (and reassigning) an already-local name. */
static int shell_builtins__local_track(shell_state_t *state, const char *name) {
    shell_local_frame_t *frame = state->local_frame;
    for (size_t i = 0; i < frame->count; ++i) {
        if (strcmp(frame->entries[i].name, name) == 0) return 0;
    }
    if (frame->count >= SHELL__MAX_VARIABLES) {
        stdio__printf("shell: local: too many local variables\n");
        return 1;
    }
    if (frame->count >= frame->capacity) {
        size_t new_capacity = frame->capacity == 0 ? 4 : frame->capacity * 2;
        if (new_capacity > SHELL__MAX_VARIABLES) new_capacity = SHELL__MAX_VARIABLES;
        shell_local_entry_t *grown = memory__realloc(frame->entries, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            stdio__printf("shell: out of memory\n");
            return 1;
        }
        frame->entries = grown;
        frame->capacity = new_capacity;
    }
    char *name_copy = shell_builtins__dup(name, strlen(name));
    if (name_copy == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    const char *previous = shell_builtins__get(state, name);
    char *previous_copy = NULL;
    if (previous != NULL) {
        previous_copy = shell_builtins__dup(previous, strlen(previous));
        if (previous_copy == NULL) {
            memory__free(name_copy);
            stdio__printf("shell: out of memory\n");
            return 1;
        }
    }
    frame->entries[frame->count].name = name_copy;
    frame->entries[frame->count].previous_value = previous_copy;
    frame->count++;
    return 0;
}

/* `local NAME[=value]...`: only valid while a function call is executing --
 * state->local_frame is set up around the body's run by
 * shell_compound__call_function() (see shell_local_frame_t in
 * shell_internal.h) and is NULL at top level, the same boundary
 * state->positional already uses. Declares each NAME as shadowing whatever
 * it held before -- or as freshly unset, if it wasn't set at all -- for the
 * rest of this call, same as bash. A bare NAME (no "=value") behaves like
 * bash's own "local x": it starts out empty, distinct from whatever a
 * same-named variable outside this call might hold. */
static int shell_builtins__local(shell_state_t *state, int argc, char **argv) {
    if (state->local_frame == NULL) {
        stdio__printf("shell: local: can only be used inside a function\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        char *equals = strchr(argv[i], '=');
        const char *name = argv[i];
        char name_buffer[SHELL__VARIABLE_NAME_MAX];
        if (equals != NULL) {
            size_t length = (size_t)(equals - argv[i]);
            if (length >= sizeof(name_buffer) || !shell_parser__valid_name(argv[i], length)) {
                stdio__printf("shell: invalid variable name\n");
                return 2;
            }
            memcpy(name_buffer, argv[i], length);
            name_buffer[length] = '\0';
            name = name_buffer;
        } else if (!shell_parser__valid_name(name, strlen(name))) {
            stdio__printf("shell: invalid variable name\n");
            return 2;
        }
        int status = shell_builtins__local_track(state, name);
        if (status != 0) return status;
        status = shell_builtins__set(state, name, equals != NULL ? equals + 1 : "");
        if (status != 0) return status;
    }
    return 0;
}

/* The three trap slots this shell tracks -- see state->trap_exit/trap_int/
 * trap_term in shell_internal.h. EXIT is bash's own pseudo-signal name for
 * "this shell/script run is ending", not a real bruce_process_signal_t. */
typedef enum {
    SHELL_TRAP_EXIT,
    SHELL_TRAP_INT,
    SHELL_TRAP_TERM,
} shell_trap_kind_t;

/* Resolves a `trap` SIGSPEC to one of the three slots above: "EXIT" or "0"
 * for the pseudo-signal, or INT/TERM by name (optional "SIG" prefix,
 * case-insensitive) or number -- the same acceptance
 * shell_jobs__parse_signal() gives `kill`'s SIGSPEC. KILL is deliberately
 * never accepted here: it isn't catchable by a real process either, so
 * `trap ... KILL` has nothing to hook -- rejected the same way bash's own
 * trap rejects it. */
static bool shell_builtins__parse_trap_signal(const char *text, shell_trap_kind_t *out_kind) {
    if (text == NULL || text[0] == '\0') return false;
    if (strcmp(text, "0") == 0) {
        *out_kind = SHELL_TRAP_EXIT;
        return true;
    }
    const char *name = text;
    if (strncasecmp(name, "SIG", 3) == 0 && name[3] != '\0') name += 3;
    if (strcasecmp(name, "EXIT") == 0) {
        *out_kind = SHELL_TRAP_EXIT;
        return true;
    }
    if (strcasecmp(name, "INT") == 0) {
        *out_kind = SHELL_TRAP_INT;
        return true;
    }
    if (strcasecmp(name, "TERM") == 0) {
        *out_kind = SHELL_TRAP_TERM;
        return true;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0') return false;
    if (value == BRUCE_PROCESS_SIGNAL_INT) {
        *out_kind = SHELL_TRAP_INT;
        return true;
    }
    if (value == BRUCE_PROCESS_SIGNAL_TERM) {
        *out_kind = SHELL_TRAP_TERM;
        return true;
    }
    return false;
}

static const char *shell_builtins__trap_name(shell_trap_kind_t kind) {
    switch (kind) {
        case SHELL_TRAP_EXIT: return "EXIT";
        case SHELL_TRAP_INT: return "INT";
        case SHELL_TRAP_TERM: return "TERM";
    }
    return "";
}

static char **shell_builtins__trap_slot(shell_state_t *state, shell_trap_kind_t kind) {
    switch (kind) {
        case SHELL_TRAP_EXIT: return &state->trap_exit;
        case SHELL_TRAP_INT: return &state->trap_int;
        case SHELL_TRAP_TERM: return &state->trap_term;
    }
    return NULL;
}

/* `trap` (bare): lists every trap currently set, bash's own "trap -p"
 * format ("trap -- 'ACTION' SIGSPEC" per line) -- a slot that's NULL (no
 * trap set, the default disposition) is skipped entirely, but one that's ""
 * (explicitly ignored via `trap '' SIGSPEC`) is still listed, matching
 * bash's own distinction between the two.
 * `trap -l`: lists the signal names this trap implementation understands.
 * `trap ACTION SIGSPEC...`: sets ACTION (run through shell_compound__run()
 * verbatim when the trap fires) for every SIGSPEC given; ACTION == "-"
 * resets those slots back to the default instead of setting a literal "-"
 * action, matching bash. */
static int shell_builtins__trap(shell_state_t *state, int argc, char **argv) {
    if (argc == 1) {
        static const shell_trap_kind_t kinds[] = {SHELL_TRAP_EXIT, SHELL_TRAP_INT, SHELL_TRAP_TERM};
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
            char **slot = shell_builtins__trap_slot(state, kinds[i]);
            if (*slot == NULL) continue;
            stdio__printf("trap -- '%s' %s\n", *slot, shell_builtins__trap_name(kinds[i]));
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-l") == 0) {
        stdio__printf("EXIT\nINT\nTERM\n");
        return 0;
    }
    if (argc < 3) {
        stdio__printf("shell: trap: usage: trap [-l] [ACTION SIGSPEC...]\n");
        return 2;
    }
    const char *action = argv[1];
    bool reset = strcmp(action, "-") == 0;
    for (int i = 2; i < argc; ++i) {
        shell_trap_kind_t kind;
        if (!shell_builtins__parse_trap_signal(argv[i], &kind)) {
            stdio__printf("shell: trap: %s: invalid signal specification\n", argv[i]);
            return 2;
        }
        char **slot = shell_builtins__trap_slot(state, kind);
        memory__free(*slot);
        if (reset) {
            *slot = NULL;
            continue;
        }
        *slot = shell_builtins__dup(action, strlen(action));
        if (*slot == NULL) {
            stdio__printf("shell: out of memory\n");
            return 1;
        }
    }
    return 0;
}

void shell_builtins__fire_exit_trap(shell_state_t *state) {
    if (state->trap_exit_fired || state->trap_exit == NULL || state->trap_exit[0] == '\0') {
        state->trap_exit_fired = true;
        return;
    }
    state->trap_exit_fired = true;
    bool was_exit_requested = state->exit_requested;
    int saved_status = state->exit_status;
    /* Cleared so shell_compound__run_sequence() actually executes the trap
     * body instead of immediately unwinding as if this run were still
     * ending from whatever set exit_requested before this call. */
    state->exit_requested = false;
    (void)shell_compound__run(state, state->trap_exit, NULL, 0);
    if (!state->exit_requested) {
        /* The trap action didn't itself call `exit` -- restore the status
         * this run was already ending with before the trap fired. */
        state->exit_requested = was_exit_requested;
        state->exit_status = saved_status;
    }
    /* else: the trap action called `exit M` itself -- exit_requested/
     * exit_status already hold that, exactly the override bash gives an
     * EXIT trap that calls exit. */
}

bool shell_builtins__fire_signal_trap(shell_state_t *state, bruce_process_signal_t signal) {
    shell_trap_kind_t kind;
    if (signal == BRUCE_PROCESS_SIGNAL_INT) {
        kind = SHELL_TRAP_INT;
    } else if (signal == BRUCE_PROCESS_SIGNAL_TERM) {
        kind = SHELL_TRAP_TERM;
    } else {
        return false;
    }
    char **slot = shell_builtins__trap_slot(state, kind);
    if (*slot == NULL) return false;
    (void)process__clear_signal();
    if ((*slot)[0] != '\0') (void)shell_compound__run(state, *slot, NULL, 0);
    return true;
}

/* Decodes printf-style backslash escapes: `\\`, `\a`, `\b`, `\e`, `\f`,
 * `\n`, `\r`, `\t`, `\v`, `\"` (kept as a literal quote, so a shell-quoted
 * FORMAT that embeds one round-trips), `\NNN` (1-3 octal digits) and `\xHH`
 * (1-2 hex digits). An unrecognized backslash sequence -- including a
 * trailing lone `\` -- is left exactly as written (backslash and all)
 * rather than being treated as an error, matching bash's own leniency here.
 * Reused for both FORMAT itself (once, up front) and each `%b` ARGUMENT.
 * Writes into `dest`, which the caller must size to at least `length` bytes
 * -- every case here consumes at least as many source bytes as it emits --
 * and returns the decoded length (`dest` is *not* NUL-terminated). */
static size_t shell_builtins__printf_unescape(const char *src, size_t length, char *dest) {
    size_t out = 0;
    for (size_t i = 0; i < length; ++i) {
        if (src[i] != '\\' || i + 1 >= length) {
            dest[out++] = src[i];
            continue;
        }
        char c = src[++i];
        switch (c) {
            case '\\': dest[out++] = '\\'; break;
            case 'a': dest[out++] = '\a'; break;
            case 'b': dest[out++] = '\b'; break;
            case 'e': dest[out++] = '\033'; break;
            case 'f': dest[out++] = '\f'; break;
            case 'n': dest[out++] = '\n'; break;
            case 'r': dest[out++] = '\r'; break;
            case 't': dest[out++] = '\t'; break;
            case 'v': dest[out++] = '\v'; break;
            case '"': dest[out++] = '"'; break;
            case 'x': {
                int value = 0, digits = 0;
                while (digits < 2 && i + 1 < length && isxdigit((unsigned char)src[i + 1])) {
                    char h = src[++i];
                    int nibble = isdigit((unsigned char)h) ? h - '0' : tolower((unsigned char)h) - 'a' + 10;
                    value = value * 16 + nibble;
                    digits++;
                }
                if (digits == 0) {
                    dest[out++] = '\\';
                    dest[out++] = 'x';
                } else {
                    dest[out++] = (char)value;
                }
                break;
            }
            default:
                if (c >= '0' && c <= '7') {
                    int value = c - '0', digits = 1;
                    while (digits < 3 && i + 1 < length && src[i + 1] >= '0' && src[i + 1] <= '7') {
                        value = value * 8 + (src[++i] - '0');
                        digits++;
                    }
                    dest[out++] = (char)value;
                } else {
                    dest[out++] = '\\';
                    dest[out++] = c;
                }
                break;
        }
    }
    return out;
}

/* Parses one printf-style conversion starting at format[*i] == '%':
 * optional flags (`-+ 0#`), optional width, optional `.` + precision, then
 * the conversion character itself -- `*` (dynamic width/precision pulled
 * from an argument) isn't recognized as a flag/digit, so a specifier using
 * it falls through to the "invalid" return below, same as any other
 * unsupported conversion character. On success, copies the whole
 * specifier -- leading '%' through the conversion character -- into `spec`
 * (NUL-terminated) and returns the conversion character; advances *i to
 * just past it. Returns 0 (and still advances *i, past whatever it did
 * recognize) if the specifier is malformed or its conversion character
 * isn't one of "diouxXcsb%". */
static char shell_builtins__printf_spec(const char *format, size_t length, size_t *i, char *spec, size_t spec_cap) {
    size_t start = *i;
    size_t pos = start + 1;
    /* format[pos] != '\0' guards every strchr() below against its one
     * gotcha: strchr(s, '\0') always "matches" (the needle string's own
     * terminator), which would otherwise treat an embedded NUL byte -- from
     * a `\0` octal escape decoded into the middle of FORMAT -- as a valid
     * flag or conversion character instead of the garbage byte it is. */
    while (pos < length && format[pos] != '\0' && strchr("-+ 0#", format[pos]) != NULL) pos++;
    while (pos < length && isdigit((unsigned char)format[pos])) pos++;
    if (pos < length && format[pos] == '.') {
        pos++;
        while (pos < length && isdigit((unsigned char)format[pos])) pos++;
    }
    if (pos >= length || format[pos] == '\0' || strchr("diouxXcsb%", format[pos]) == NULL) {
        *i = pos < length ? pos + 1 : pos;
        return 0;
    }
    char conv = format[pos];
    size_t spec_len = pos - start + 1;
    *i = pos + 1;
    if (spec_len + 1 > spec_cap) return 0; /* absurd width/precision -- bail rather than overflow spec */
    memcpy(spec, format + start, spec_len);
    spec[spec_len] = '\0';
    return conv;
}

/* `strtol(..., 0)`, same base-0 (plain decimal or a 0x/0-prefixed literal)
 * convention shell_arith.c's own number parsing uses; an empty or entirely
 * non-numeric ARG is silently 0 rather than an error, same leniency
 * shell_arith.c already applies elsewhere. */
static long shell_builtins__printf_parse_long(const char *text) {
    if (text == NULL || text[0] == '\0') return 0;
    char *end = NULL;
    return strtol(text, &end, 0);
}

/* `printf FORMAT [ARGUMENT...]`: bash-style formatted output. FORMAT's
 * backslash escapes are decoded once up front (see
 * shell_builtins__printf_unescape()), then scanned left to right, copying
 * literal text through unchanged and consuming one ARGUMENT per conversion
 * (`%%` consumes none). If ARGUMENTs remain once FORMAT is exhausted, the
 * whole of FORMAT runs again against them, repeating until every ARGUMENT
 * has been consumed -- bash's own recycling behavior -- but only when
 * FORMAT contains at least one argument-consuming conversion; otherwise it
 * always runs exactly once no matter how many ARGUMENTs were given, same as
 * bash ("printf hi extra" prints "hi" once, not twice). A conversion with
 * no ARGUMENT left to consume (either because none were given at all, or
 * this is the last, short pass of a recycle) uses "" (or, for the numeric
 * conversions, the same string parsed as 0) rather than erroring, matching
 * bash's own "not enough arguments" fallback. Supports `%s`, `%b` (like
 * `%s`, but backslash-decodes the argument first), `%c`, `%d`/`%i`, `%o`,
 * `%u`, `%x`/`%X`, and `%%`, each with the usual flags/width/precision
 * (parsed but not interpreted here -- passed straight through to the
 * underlying vsnprintf via a per-conversion sub-format built from the
 * matched specifier). Not implemented: `-v NAME` (assign into a variable
 * instead of printing), floating-point conversions (`%e/%f/%g` and
 * friends), `%(FORMAT)T` (strftime), and `*` dynamic width/precision. */
static int shell_builtins__printf(int argc, char **argv) {
    if (argc < 2) {
        stdio__printf("shell: printf: usage: printf FORMAT [ARGUMENT...]\n");
        return 2;
    }
    const char *raw_format = argv[1];
    size_t raw_length = strlen(raw_format);
    char *format = memory__malloc(raw_length + 1);
    if (format == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    size_t format_length = shell_builtins__printf_unescape(raw_format, raw_length, format);
    format[format_length] = '\0';

    int arg_index = 2;
    bool any_conversion = false;
    int status = 0;
    int consumed_before_pass;
    do {
        consumed_before_pass = arg_index;
        for (size_t i = 0; i < format_length;) {
            if (format[i] != '%') {
                (void)stdio__write(&format[i], 1);
                i++;
                continue;
            }
            size_t start = i;
            char spec[32];
            char conv = shell_builtins__printf_spec(format, format_length, &i, spec, sizeof(spec));
            if (conv == 0) {
                stdio__printf("shell: printf: %.*s: invalid format\n", (int)(i - start), format + start);
                status = 1;
                goto done;
            }
            if (conv == '%') {
                (void)stdio__write("%", 1);
                continue;
            }
            any_conversion = true;
            const char *arg = arg_index < argc ? argv[arg_index++] : "";
            char sub_spec[40];
            size_t spec_length = strlen(spec);
            switch (conv) {
                case 's': stdio__printf(spec, arg); break;
                case 'b': {
                    size_t arg_length = strlen(arg);
                    char *decoded = memory__malloc(arg_length + 1);
                    if (decoded == NULL) {
                        stdio__printf("shell: out of memory\n");
                        status = 1;
                        goto done;
                    }
                    size_t decoded_length = shell_builtins__printf_unescape(arg, arg_length, decoded);
                    decoded[decoded_length] = '\0';
                    memcpy(sub_spec, spec, spec_length - 1);
                    sub_spec[spec_length - 1] = 's';
                    sub_spec[spec_length] = '\0';
                    stdio__printf(sub_spec, decoded);
                    memory__free(decoded);
                    break;
                }
                /* An empty ARG has no "first character" -- bash's own %c
                 * prints nothing for one rather than a literal NUL byte. */
                case 'c':
                    if (arg[0] != '\0') stdio__printf(spec, (int)(unsigned char)arg[0]);
                    break;
                case 'd':
                case 'i':
                    memcpy(sub_spec, spec, spec_length - 1);
                    sub_spec[spec_length - 1] = 'l';
                    sub_spec[spec_length] = conv;
                    sub_spec[spec_length + 1] = '\0';
                    stdio__printf(sub_spec, shell_builtins__printf_parse_long(arg));
                    break;
                case 'o':
                case 'u':
                case 'x':
                case 'X':
                    memcpy(sub_spec, spec, spec_length - 1);
                    sub_spec[spec_length - 1] = 'l';
                    sub_spec[spec_length] = conv;
                    sub_spec[spec_length + 1] = '\0';
                    stdio__printf(sub_spec, (unsigned long)shell_builtins__printf_parse_long(arg));
                    break;
                default: break; /* unreachable -- shell_builtins__printf_spec() only returns "diouxXcsb%" */
            }
        }
    } while (any_conversion && arg_index < argc && arg_index > consumed_before_pass);
done:
    memory__free(format);
    return status;
}

bool shell_builtins__is_builtin(const char *name) {
    for (size_t i = 0; i < sizeof(s_shell_builtins) / sizeof(s_shell_builtins[0]); ++i) {
        if (strcmp(name, s_shell_builtins[i].name) == 0) return true;
    }
    return false;
}

size_t shell_builtins__count(void) {
    return sizeof(s_shell_builtins) / sizeof(s_shell_builtins[0]);
}

const char *shell_builtins__name(size_t index) {
    return index < shell_builtins__count() ? s_shell_builtins[index].name : NULL;
}

const char *shell_builtins__description(size_t index) {
    return index < shell_builtins__count() ? s_shell_builtins[index].description : NULL;
}

int shell_builtins__run(shell_state_t *state, int argc, char **argv) {
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; ++i) stdio__printf("%s%s", i > 1 ? " " : "", argv[i]);
        stdio__printf("\n");
        return 0;
    }
    if (strcmp(argv[0], "printf") == 0) return shell_builtins__printf(argc, argv);
    if (strcmp(argv[0], "true") == 0) return 0;
    if (strcmp(argv[0], "false") == 0) return 1;
    if (strcmp(argv[0], "test") == 0 || strcmp(argv[0], "[") == 0 || strcmp(argv[0], "[[") == 0) {
        return shell_condition__run(state, argc, argv);
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
            shell_builtins__unset(state, argv[arg]);
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
    if (strcmp(argv[0], "continue") == 0) {
        if (argc > 2) {
            stdio__printf("shell: continue: too many arguments\n");
            return 2;
        }
        long levels = 1;
        if (argc == 2) {
            char *end = NULL;
            errno = 0;
            levels = strtol(argv[1], &end, 10);
            if (errno != 0 || end == argv[1] || *end != '\0' || levels < 1 || levels > INT_MAX) {
                stdio__printf("shell: continue: numeric argument required\n");
                return 2;
            }
        }
        /* Only meaningful inside a for/while loop -- shell_compound__run()
         * catches (and warns about) a continue_requested that outlives
         * every loop it could apply to, the same way it does for
         * break_requested. */
        state->continue_requested = (int)levels;
        return 0;
    }
    if (strcmp(argv[0], "return") == 0) {
        if (argc > 2) {
            stdio__printf("shell: return: too many arguments\n");
            return 2;
        }
        int status = state->last_status;
        if (argc == 2) {
            char *end = NULL;
            errno = 0;
            long parsed = strtol(argv[1], &end, 10);
            if (errno != 0 || end == argv[1] || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
                stdio__printf("shell: return: numeric argument required\n");
                return 2;
            }
            status = (int)((unsigned long)parsed & 0xffu);
        }
        /* Only meaningful inside a function call -- state->local_frame is
         * only ever non-NULL for the duration of
         * shell_compound__call_function()'s own body run (see its doc
         * comment), the same signal `local` already relies on to find "the
         * currently executing call". Unlike break/continue, this is checked
         * (and rejected) right here rather than deferred to some later
         * unwind-boundary check: a stray "return" has nothing to propagate
         * into in the first place. */
        if (state->local_frame == NULL) {
            stdio__printf("shell: return: can only `return' from a function\n");
            return 1;
        }
        state->return_requested = true;
        return status;
    }
    if (strcmp(argv[0], "shift") == 0) {
        if (argc > 2) {
            stdio__printf("shell: shift: too many arguments\n");
            return 2;
        }
        long count = 1;
        if (argc == 2) {
            char *end = NULL;
            errno = 0;
            count = strtol(argv[1], &end, 10);
            if (errno != 0 || end == argv[1] || *end != '\0' || count < 0 || count > INT_MAX) {
                stdio__printf("shell: shift: numeric argument required\n");
                return 2;
            }
        }
        if (count == 0) return 0;
        if (count > state->positional_count) {
            stdio__printf("shell: shift: shift count out of range\n");
            return 1;
        }
        for (long i = 0; i < count; ++i) memory__free(state->positional[i]);
        int remaining = state->positional_count - (int)count;
        for (int i = 0; i < remaining; ++i) state->positional[i] = state->positional[i + (int)count];
        state->positional_count = remaining;
        return 0;
    }
    if (strcmp(argv[0], "read") == 0) return shell_builtins__read(state, argc, argv);
    if (strcmp(argv[0], "local") == 0) return shell_builtins__local(state, argc, argv);
    if (strcmp(argv[0], "jobs") == 0) return shell_jobs__run(state, argc, argv);
    if (strcmp(argv[0], "wait") == 0) return shell_jobs__wait(state, argc, argv);
    if (strcmp(argv[0], "kill") == 0) return shell_jobs__kill(state, argc, argv);
    if (strcmp(argv[0], "fg") == 0) return shell_jobs__fg(state, argc, argv);
    if (strcmp(argv[0], "bg") == 0) return shell_jobs__bg(state, argc, argv);
    if (strcmp(argv[0], "disown") == 0) return shell_jobs__disown(state, argc, argv);
    if (strcmp(argv[0], "trap") == 0) return shell_builtins__trap(state, argc, argv);
    /* "time" is intercepted in shell_executor__dispatch() before it ever
     * reaches here (it needs to wrap the function/builtin/external dispatch
     * itself), so it's listed for documentation purposes only -- this branch
     * is otherwise unreachable for it. */
    stdio__printf(
        "Builtins: echo true false cd set unset export clear reset help exit test [ [[ break read time local jobs "
        "wait\n"
    );
    stdio__printf("Operators: ; && || and producer | text. Redirection: > >> < <<DELIM.\n");
    stdio__printf("Compound: if/elif/else/fi, for, while, ((...)) arithmetic, functions.\n");
    return 0;
}
