#include "shell_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"
#include "shell_compound.h"
#include "shell_builtins.h"
#include "shell_console.h"
#include "shell_history.h"
#include "shell_internal.h"

/* Keeps $COLUMNS/$LINES in sync with the routed session's terminal size
 * (see core_sdk/tty.h). Cheap no-op once nothing has changed, so it's safe
 * to call before every interactive read -- that's the only way a resize
 * becomes visible without a SIGWINCH equivalent (see tty.h's generation
 * counter). Also a no-op when the session has no known size at all (a
 * plain pipe, or the physical serial console), same as a real tty check. */
static void shell__sync_tty_size(shell_state_t *state) {
    bruce_tty_size_t size;
    if (!tty__isatty() || tty__get_size(&size) != BRUCE_OK || size.generation == state->tty_generation) return;
    state->tty_generation = size.generation;
    char columns[8];
    char rows[8];
    snprintf(columns, sizeof(columns), "%u", (unsigned)size.columns);
    snprintf(rows, sizeof(rows), "%u", (unsigned)size.rows);
    if (shell_builtins__set(state, "COLUMNS", columns) == 0) (void)shell_builtins__export(state, "COLUMNS");
    if (shell_builtins__set(state, "LINES", rows) == 0) (void)shell_builtins__export(state, "LINES");
}

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
    const char *working_directory = shell_builtins__get(state, "PWD");
    if (working_directory == NULL || working_directory[0] != '/') {
        if (shell_builtins__set(state, "PWD", "/") == 0) {
            (void)shell_builtins__export(state, "PWD");
        }
    }
    shell__sync_tty_size(state);
}

void shell__state_free(shell_state_t *state) {
    if (state == NULL) return;
    shell_compound__state_free(state);
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
    return shell_compound__run(state, line);
}

/* Appends one physical line to the multi-line accumulation buffer that both
 * shell__run_script() and shell__interactive() feed shell_compound__pending()
 * -- joining lines with a real '\n' so an if/fi or function block that spans
 * several of them parses as one unit (see the '\n' connector case
 * shell_parser__plan() gained for this). Returns false if `line` wouldn't
 * fit within `capacity`. */
static bool shell__block_append(char *block, size_t capacity, size_t *block_used, const char *line, size_t line_len) {
    size_t separator = *block_used > 0 ? 1u : 0u;
    if (*block_used + separator + line_len + 1 > capacity) return false;
    if (separator != 0) block[(*block_used)++] = '\n';
    memcpy(block + *block_used, line, line_len);
    *block_used += line_len;
    block[*block_used] = '\0';
    return true;
}

static int shell__run_script(shell_state_t *state, const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t opened = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (opened != BRUCE_OK) {
        stdio__printf("shell: %s: cannot open (%d)\n", path, opened);
        return 1;
    }
    char *line = memory__malloc(SHELL__LINE_MAX);
    char *block = memory__malloc(SHELL__BLOCK_MAX);
    if (line == NULL || block == NULL) {
        stdio__printf("shell: out of memory\n");
        memory__free(line);
        memory__free(block);
        (void)storage__close(file);
        return 1;
    }
    size_t used = 0;
    size_t block_used = 0;
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
                if (!shell__block_append(block, SHELL__BLOCK_MAX, &block_used, line, used)) {
                    stdio__printf("shell: %s: script block too long\n", path);
                    status = 2;
                    goto done;
                }
                used = 0;
                if (!shell_compound__pending(block)) {
                    status = shell__execute_line(state, block);
                    block_used = 0;
                    block[0] = '\0';
                }
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
    } else if (!state->exit_requested && (used > 0 || block_used > 0)) {
        if (used > 0) {
            if (line[used - 1] == '\r') used--;
            if (!shell__block_append(block, SHELL__BLOCK_MAX, &block_used, line, used)) {
                stdio__printf("shell: %s: script block too long\n", path);
                status = 2;
                goto done;
            }
        }
        if (shell_compound__pending(block)) {
            stdio__printf("shell: %s: unexpected end of file (unterminated if/function)\n", path);
            status = 2;
        } else {
            status = shell__execute_line(state, block);
        }
    }
done:
    memory__free(line);
    memory__free(block);
    (void)storage__close(file);
    return state->exit_requested ? state->exit_status : status;
}

static int shell__interactive(shell_state_t *state, bool suppress_echo) {
    char *line = memory__malloc(SHELL__LINE_MAX);
    char *block = memory__malloc(SHELL__BLOCK_MAX);
    if (line == NULL || block == NULL) {
        stdio__printf("shell: out of memory\n");
        memory__free(line);
        memory__free(block);
        return 1;
    }
    size_t block_used = 0;
    bool skip_lf = false;
    while (!state->exit_requested) {
        shell__sync_tty_size(state);
        int length;
        if (suppress_echo) {
            length = stdio__read_line(line, SHELL__LINE_MAX, true);
        } else {
            /* Once a line has been folded into a still-incomplete block (an
             * open "if" or function "{"), switch to the "> " continuation
             * prompt -- same idea as bash's PS2 -- until it closes. */
            const char *prompt = block_used > 0 ? shell_console__continuation_prompt() : NULL;
            length = shell_console__read_line(line, SHELL__LINE_MAX, &skip_lf, prompt);
        }
        if (length == BRUCE_ERR_CANCELLED) {
            int status = 128 + (int)process__current_signal();
            memory__free(line);
            memory__free(block);
            return status;
        }
        if (length < 0) break;
        if (length > 0) (void)shell_history__append(SHELL_HISTORY_PATH, line);
        if (!shell__block_append(block, SHELL__BLOCK_MAX, &block_used, line, (size_t)length)) {
            stdio__printf("shell: input too long\n");
            block_used = 0;
            block[0] = '\0';
            continue;
        }
        if (!shell_compound__pending(block)) {
            (void)shell__execute_line(state, block);
            block_used = 0;
            block[0] = '\0';
        }
    }
    if (!state->exit_requested && block_used > 0) {
        if (shell_compound__pending(block)) {
            stdio__printf("shell: unexpected end of input (unterminated if/function)\n");
        } else {
            (void)shell__execute_line(state, block);
        }
    }
    memory__free(line);
    memory__free(block);
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
