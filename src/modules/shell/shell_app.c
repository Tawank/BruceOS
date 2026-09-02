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
#include "shell_executor.h"
#include "shell_history.h"
#include "shell_internal.h"
#include "shell_parser.h"

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
    return shell_compound__run(state, line, NULL, 0);
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

/* Safety cap on how many "<<DELIM" heredocs a single accumulated block (see
 * shell__block_append() above) may contain -- generous for any realistic
 * script, and matched in spirit to SHELL__MAX_COMMANDS. Heredocs are
 * currently only collected by shell__run_script() below, not by
 * shell__interactive()'s own read loop further down -- typing "<<EOF" at the
 * interactive prompt reaches shell_parser__plan() with no body collected for
 * it and reports "heredoc ... not supported here", the same message a
 * heredoc inside a re-parsed function body gets (see shell_compound.c's
 * shell_compound__call_function()). */
#define SHELL_APP__MAX_HEREDOCS 8

/* Reads one more raw line into `out` (`out_capacity` bytes, NUL-terminated,
 * a trailing '\r' stripped same as shell__run_script()'s own line handling),
 * pulling bytes from `*chunk_pos`/`*chunk_size` within `chunk` and refilling
 * via storage__read() on `file` as needed -- the very same byte cursor
 * shell__run_script()'s own reading loop advances, shared by pointer so
 * control returns there exactly where this function leaves off, whether
 * that's mid-chunk or needing a fresh read. Used only while collecting a
 * heredoc body (see shell_app__collect_heredoc_body() below), where lines
 * must be read raw -- never joined into `block` or checked against
 * shell_compound__pending() -- until the delimiter line turns up. Returns
 * the line's length (never counting a line that doesn't fit -- see
 * *overlong), or -1 at real end-of-file with nothing left to read at all. */
static int shell_app__heredoc_read_line(
    bruce_file_id_t file, char *chunk, size_t chunk_capacity, size_t *chunk_pos, size_t *chunk_size, char *out,
    size_t out_capacity, bool *overlong
) {
    size_t used = 0;
    for (;;) {
        if (*chunk_pos >= *chunk_size) {
            bruce_result_t read = storage__read(file, chunk, chunk_capacity, chunk_size);
            *chunk_pos = 0;
            if (read != BRUCE_OK || *chunk_size == 0) {
                if (used == 0) return -1;
                if (used < out_capacity) out[used] = '\0';
                return (int)used;
            }
        }
        char c = chunk[(*chunk_pos)++];
        if (c == '\n') {
            if (used > 0 && out[used - 1] == '\r') used--;
            if (used < out_capacity) out[used] = '\0';
            return (int)used;
        }
        if (used + 1 < out_capacity) out[used++] = c;
        else *overlong = true;
    }
}

/* Collects a heredoc's raw (not yet $expanded) body: repeatedly reads a raw
 * line via shell_app__heredoc_read_line() above and appends it (plus a real
 * '\n') to a growing memory__malloc()-owned string, until a line matches
 * `delim`/`delim_len` -- after first stripping that line's own leading tabs
 * too when `strip_tabs` is set ("<<-"), matching bash's own rule that a
 * tab-indented terminator still closes the heredoc. *out_raw becomes the
 * caller-owned (memory__free()) body text, excluding the terminator line
 * itself. Returns 0 on success, 1 if the terminator is never found before
 * end-of-file, or 2 on a hard error (a body line too long, or the body
 * outgrowing SHELL__HEREDOC_MAX, or out of memory).
 *
 * `raw_line`/`raw_line_capacity` is scratch space for one line at a time --
 * caller-owned (memory__malloc()) rather than a local SHELL__LINE_MAX array
 * here, since the "shell" task's stack budget (SHELL_STACK_BYTES in main.c)
 * is tight enough that this function's own frame plus shell__run_script()'s
 * already caused a stack overflow before this was moved to the heap. */
static int shell_app__collect_heredoc_body(
    bruce_file_id_t file, char *chunk, size_t chunk_capacity, size_t *chunk_pos, size_t *chunk_size,
    bool strip_tabs, const char *delim, size_t delim_len, char *raw_line, size_t raw_line_capacity, char **out_raw
) {
    char *body = memory__malloc(1);
    if (body == NULL) return 2;
    body[0] = '\0';
    size_t body_len = 0;
    for (;;) {
        bool overlong = false;
        int length = shell_app__heredoc_read_line(
            file, chunk, chunk_capacity, chunk_pos, chunk_size, raw_line, raw_line_capacity, &overlong
        );
        if (overlong) {
            memory__free(body);
            return 2;
        }
        if (length < 0) {
            memory__free(body);
            return 1;
        }
        const char *compare = raw_line;
        size_t compare_len = (size_t)length;
        if (strip_tabs) {
            while (compare_len > 0 && *compare == '\t') {
                compare++;
                compare_len--;
            }
        }
        if (compare_len == delim_len && memcmp(compare, delim, delim_len) == 0) {
            *out_raw = body;
            return 0;
        }
        size_t needed = body_len + compare_len + 2u; /* the line, a '\n', and the NUL */
        if (needed > SHELL__HEREDOC_MAX) {
            memory__free(body);
            return 2;
        }
        char *grown = memory__realloc(body, needed);
        if (grown == NULL) {
            memory__free(body);
            return 2;
        }
        body = grown;
        memcpy(body + body_len, compare, compare_len);
        body_len += compare_len;
        body[body_len++] = '\n';
        body[body_len] = '\0';
    }
}

/* Frees every entry collected so far in `heredoc_bodies[0..*heredoc_count)`
 * and resets the count to 0 -- shared by every point shell__run_script()
 * finishes with one accumulated block's heredocs, success or error alike. */
static void shell_app__heredoc_bodies_free(char **heredoc_bodies, size_t *heredoc_count) {
    for (size_t i = 0; i < *heredoc_count; ++i) memory__free(heredoc_bodies[i]);
    *heredoc_count = 0;
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
    /* Heredoc scratch space, heap-allocated for the same stack-budget reason
     * as shell_app__collect_heredoc_body()'s own `raw_line` parameter (see
     * its doc comment) -- `delim_copy` holds one heredoc delimiter at a time
     * (copied out of `line` before it's reused, see below) and `raw_line` is
     * handed down to shell_app__collect_heredoc_body() as its line-at-a-time
     * scratch buffer. */
    char *delim_copy = memory__malloc(SHELL__LINE_MAX);
    char *raw_line = memory__malloc(SHELL__LINE_MAX);
    if (line == NULL || block == NULL || delim_copy == NULL || raw_line == NULL) {
        stdio__printf("shell: out of memory\n");
        memory__free(line);
        memory__free(block);
        memory__free(delim_copy);
        memory__free(raw_line);
        (void)storage__close(file);
        return 1;
    }
    size_t used = 0;
    size_t block_used = 0;
    int status = state->last_status;
    bool overlong = false;
    /* Heredoc bodies collected for whatever block is currently being
     * accumulated -- reset (freed) every time a block finishes, whether by
     * running it or by an error abandoning it (see `goto done` below, and
     * shell_app__heredoc_bodies_free()'s own doc comment). */
    char *heredoc_bodies[SHELL_APP__MAX_HEREDOCS] = {0};
    size_t heredoc_count = 0;
    char chunk[128];
    size_t chunk_pos = 0;
    size_t chunk_size = 0;
    for (;;) {
        if (chunk_pos >= chunk_size) {
            bruce_result_t read = storage__read(file, chunk, sizeof(chunk), &chunk_size);
            chunk_pos = 0;
            if (read != BRUCE_OK) {
                stdio__printf("shell: %s: read error (%d)\n", path, read);
                status = 1;
                break;
            }
            if (chunk_size == 0) break;
        }
        char c = chunk[chunk_pos++];
        if (c == '\n') {
            if (overlong) {
                stdio__printf("shell: %s: script line too long\n", path);
                status = 2;
                goto done;
            }
            if (used > 0 && line[used - 1] == '\r') used--;
            /* Recognized *before* the line joins `block` -- see
             * shell_parser__find_heredoc_marker()'s own doc comment for why
             * this has to happen here, ahead of ordinary block/pending
             * handling, rather than once the whole block is parsed. */
            bool strip_tabs = false, literal = false;
            const char *delim = NULL;
            size_t delim_len = 0;
            const char *marker_error = NULL;
            bool has_marker =
                shell_parser__find_heredoc_marker(line, used, &strip_tabs, &literal, &delim, &delim_len, &marker_error);
            if (marker_error != NULL) {
                stdio__printf("shell: %s: %s\n", path, marker_error);
                status = 2;
                goto done;
            }
            if (!shell__block_append(block, SHELL__BLOCK_MAX, &block_used, line, used)) {
                stdio__printf("shell: %s: script block too long\n", path);
                status = 2;
                goto done;
            }
            used = 0;
            if (has_marker) {
                if (heredoc_count >= SHELL_APP__MAX_HEREDOCS) {
                    stdio__printf("shell: %s: too many heredocs\n", path);
                    status = 2;
                    goto done;
                }
                /* `delim` points into `line`, which the raw per-char loop
                 * below is about to start refilling from index 0 again --
                 * copy it out before that happens into the heap-allocated
                 * `delim_copy` (see its allocation above for why this isn't a
                 * local array). `raw_line` is likewise heap-allocated scratch
                 * space, handed down for shell_app__collect_heredoc_body() to
                 * read each body line into. */
                if (delim_len >= SHELL__LINE_MAX) {
                    stdio__printf("shell: %s: heredoc delimiter too long\n", path);
                    status = 2;
                    goto done;
                }
                memcpy(delim_copy, delim, delim_len);
                char *raw_body = NULL;
                int collected = shell_app__collect_heredoc_body(
                    file, chunk, sizeof(chunk), &chunk_pos, &chunk_size, strip_tabs, delim_copy, delim_len, raw_line,
                    SHELL__LINE_MAX, &raw_body
                );
                if (collected == 1) {
                    stdio__printf("shell: %s: unexpected end of file (unterminated heredoc)\n", path);
                    status = 2;
                    goto done;
                }
                if (collected != 0) {
                    stdio__printf("shell: %s: heredoc body too long\n", path);
                    status = 2;
                    goto done;
                }
                char *final_body = raw_body;
                if (!literal) {
                    const char *expand_error = NULL;
                    char *expanded = shell_parser__expand_text(
                        raw_body, strlen(raw_body), shell_executor__lookup, shell_executor__run_substitution,
                        shell_executor__eval_arith_word, state, state->last_status, &expand_error
                    );
                    memory__free(raw_body);
                    if (expanded == NULL) {
                        stdio__printf(
                            "shell: %s: %s\n", path, expand_error != NULL ? expand_error : "heredoc expansion failed"
                        );
                        status = 2;
                        goto done;
                    }
                    final_body = expanded;
                }
                heredoc_bodies[heredoc_count++] = final_body;
            }
            if (!shell_compound__pending(block)) {
                status = shell_compound__run(state, block, heredoc_bodies, heredoc_count);
                shell_app__heredoc_bodies_free(heredoc_bodies, &heredoc_count);
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
            status = shell_compound__run(state, block, heredoc_bodies, heredoc_count);
        }
    }
done:
    shell_app__heredoc_bodies_free(heredoc_bodies, &heredoc_count);
    memory__free(line);
    memory__free(block);
    memory__free(delim_copy);
    memory__free(raw_line);
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
            bruce_process_signal_t signal = process__current_signal();
            if (signal == BRUCE_PROCESS_SIGNAL_INT) {
                /* Like bash: Ctrl+C at the prompt just throws away whatever
                 * was being typed and shows a fresh prompt, it doesn't quit
                 * the shell. TERM/KILL (someone actually closing this shell)
                 * still fall through below and exit. */
                (void)process__clear_signal();
                /* Also discard anything already queued but not yet read (a
                 * fast paste/burst that arrived before the interrupt) -- a
                 * real tty flushes pending input on SIGINT the same way
                 * (termios' NOFLSH-unset default); without this, its
                 * leftover bytes would survive into the fresh prompt below
                 * and get replayed as if freshly typed. */
                (void)stdio__flush_input();
                (void)stdio__write("^C\r\n", 4);
                block_used = 0;
                block[0] = '\0';
                continue;
            }
            int status = 128 + (int)signal;
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
