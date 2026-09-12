#include "shell_app.h"

#include <ctype.h>
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
#include "shell_jobs.h"
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
 * shell_parser__plan() gained for this). `join_directly` skips that '\n'
 * separator, concatenating straight onto whatever's already in `block`
 * instead -- for shell__interactive()'s "cmd \" line continuation, where the
 * backslash-newline itself is dropped entirely (same as bash: "echo foo\"
 * then "bar" reads back as one "echo foobar", not two statements). Returns
 * false if `line` wouldn't fit within `capacity`. */
static bool shell__block_append(
    char *block, size_t capacity, size_t *block_used, const char *line, size_t line_len, bool join_directly
) {
    size_t separator = !join_directly && *block_used > 0 ? 1u : 0u;
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
            if (!shell__block_append(block, SHELL__BLOCK_MAX, &block_used, line, used, false)) {
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
            if (!shell__block_append(block, SHELL__BLOCK_MAX, &block_used, line, used, false)) {
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

/* Bash's "cmd \" -> keep typing on the next line: true when `line`
 * (`length` bytes) ends in an unescaped backslash -- an even run of
 * trailing backslashes is just literal backslash characters (each pair
 * cancels to one), only an odd run leaves the final one live as a
 * continuation marker. On a true match, *out_length is `length` with that
 * trailing backslash dropped -- the continuation carries no text of its
 * own, same as the newline it's absorbing ("echo foo\" then "bar" reads
 * back as one "echo foobar", not two statements). */
static bool shell_app__line_continuation_length(const char *line, size_t length, size_t *out_length) {
    size_t trailing = 0;
    while (trailing < length && line[length - 1 - trailing] == '\\') trailing++;
    if (trailing % 2 == 0) return false;
    *out_length = length - 1;
    return true;
}

/* Whether `a` (`a_len` bytes) followed by `b` (`b_len` bytes) -- `block` and
 * the not-yet-appended `line` currently being considered, kept as two spans
 * rather than requiring them already joined in one buffer -- ends inside an
 * unterminated single-quoted string. Same escape/quote bookkeeping
 * shell_compound__pending() already does for its if/for/case nesting,
 * pulled out here just for the one bit shell_app__line_continuation_length()
 * needs: a trailing "\" only means "keep typing on the next line" outside
 * single quotes, since backslash keeps no special meaning at all inside
 * them (matching bash) -- inside double quotes or fully unquoted, it's a
 * real continuation either way. Scanning `a` then `b` in one pass (instead
 * of checking `a` alone) matters for a quote opened and left unterminated
 * within `b` itself, e.g. "echo 'hi\" -- the trailing "\" there is just a
 * literal character inside the string `b` just opened, not a
 * continuation, even though `a` alone (whatever came before this line)
 * isn't inside any quote at all. */
static bool shell_app__span_ends_in_single_quote(const char *a, size_t a_len, const char *b, size_t b_len) {
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    for (int pass = 0; pass < 2; ++pass) {
        const char *text = pass == 0 ? a : b;
        size_t length = pass == 0 ? a_len : b_len;
        for (size_t i = 0; i < length; ++i) {
            char c = text[i];
            if (escaped) {
                escaped = false;
            } else if (!single && c == '\\') {
                escaped = true;
            } else if (!double_quote && c == '\'') {
                single = !single;
            } else if (!single && c == '"') {
                double_quote = !double_quote;
            }
        }
    }
    return single;
}

/* Bash keeps a multi-line if/for/while/case/{}/quoted command as a single
 * history entry; shell_history.c's file format is strictly one physical
 * line per entry (shell_history__append() writes `line` then a literal
 * '\n', and every read scans for '\n' to find entry boundaries), so a
 * block's real embedded '\n' bytes can't be written out as-is -- that would
 * read back as several unrelated entries instead of the one command that
 * was actually run. Flattens `block` (`block_len` bytes) into the one-line
 * form a user would have typed by hand: a '\n' that falls inside an open
 * quote (changing it would change the string's own value) becomes a plain
 * space; elsewhere, a '\n' right after a keyword whose body must follow
 * directly with no bare ';' allowed ("then"/"else"/"do"/"elif"/"in"/"{")
 * becomes a space too, and every other structural '\n' becomes "; " --
 * exactly how bash accepts ';' in place of a newline almost everywhere else
 * ("if true\nthen\necho hi\nfi" reads back as "if true; then echo hi; fi").
 * Doesn't special-case "# comment" the way shell_compound__pending() does --
 * a '#' has nowhere left to stop once its line's own '\n' has been folded
 * away, so treating it as a comment here would make it silently swallow
 * whatever came after it instead of just (rarely) misreading a quote
 * character inside one; the latter is the safer failure. Always
 * NUL-terminates `out`, truncating rather than overflowing if a
 * pathological block doesn't fit. */
static void shell_app__history_flatten(const char *block, size_t block_len, char *out, size_t out_capacity) {
    static const char *const no_sep_words[] = {"then", "else", "do", "elif", "in", "{"};
    if (out_capacity == 0) return;
    size_t used = 0;
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    char word[8];
    size_t word_len = 0;
    for (size_t i = 0; i < block_len && used + 1 < out_capacity; ++i) {
        char c = block[i];
        if (escaped) {
            escaped = false;
            out[used++] = c;
            if (word_len < sizeof(word)) word[word_len++] = c;
            continue;
        } else if (!single && c == '\\') {
            escaped = true;
            out[used++] = c;
            continue;
        } else if (!double_quote && c == '\'') {
            single = !single;
            out[used++] = c;
            continue;
        } else if (!single && c == '"') {
            double_quote = !double_quote;
            out[used++] = c;
            continue;
        }
        if (c == '\n' && !single && !double_quote) {
            if (word_len > 0) {
                bool no_sep = false;
                for (size_t k = 0; k < sizeof(no_sep_words) / sizeof(no_sep_words[0]); ++k) {
                    size_t wl = strlen(no_sep_words[k]);
                    if (word_len == wl && memcmp(word, no_sep_words[k], wl) == 0) {
                        no_sep = true;
                        break;
                    }
                }
                if (!no_sep && used + 1 < out_capacity) out[used++] = ';';
                if (used + 1 < out_capacity) out[used++] = ' ';
            }
            word_len = 0;
            continue;
        }
        if (c == '\n') {
            out[used++] = ' ';
            continue;
        }
        out[used++] = c;
        if (single || double_quote) continue;
        if (isspace((unsigned char)c)) word_len = 0;
        else if (word_len < sizeof(word)) word[word_len++] = c;
        else word_len = sizeof(word) + 1;
    }
    out[used] = '\0';
}

/* Records the just-completed `block` (`block_used` bytes) as one history
 * entry -- flattened to a single line first, see
 * shell_app__history_flatten() -- and runs it. Shared by
 * shell__interactive()'s main read loop and its end-of-input flush so a
 * command typed right up against Ctrl+D/EOF still gets recorded exactly
 * like one that ended with Enter. Skips the history write entirely for an
 * empty block (a blank line at a fresh prompt), same as the old
 * per-physical-line `length > 0` guard did. */
static void shell_app__interactive_execute(shell_state_t *state, const char *block, size_t block_used) {
    if (block_used > 0) {
        /* shell_app__history_flatten() never turns one input byte into more
         * than two output bytes (only an unquoted structural '\n' can
         * expand, to "; "; everything else copies 1:1 -- see its own doc
         * comment), so `block_used * 2 + 1` is an exact, provable worst-case
         * bound. Sizing to the block actually typed rather than a flat
         * SHELL__BLOCK_MAX*2 (8 KiB) matters because this runs on every
         * single command an interactive shell executes, and almost none of
         * them come anywhere near that cap. */
        size_t history_capacity = block_used * 2 + 1;
        char *history_line = memory__malloc(history_capacity);
        if (history_line != NULL) {
            shell_app__history_flatten(block, block_used, history_line, history_capacity);
            (void)shell_history__append(SHELL_HISTORY_PATH, history_line);
            memory__free(history_line);
        }
    }
    (void)shell__execute_line(state, block);
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
    /* True when the line just read ended in an unescaped "\" (see
     * shell_app__line_continuation_length() below) -- both "another physical
     * line is definitely still coming" (covers the one edge case block_used
     * alone can't: a line that's *only* a trailing backslash joins zero
     * bytes onto `block`, so block_used stays 0 even mid-continuation) and
     * "the next line read is that continuation's remainder, so join it onto
     * `block` with no separator instead of the usual real '\n'". Cleared
     * everywhere `block` itself gets cleared. */
    bool line_continued = false;
    while (!state->exit_requested) {
        shell__sync_tty_size(state);
        /* Report any "cmd &"/"func &" job that finished since the last
         * prompt, right before showing the next one -- same timing bash
         * itself uses for "[N]+ Done ...". No-op when nothing is tracked. */
        shell_jobs__poll(state);
        int length;
        if (suppress_echo) {
            length = stdio__read_line(line, SHELL__LINE_MAX, true);
        } else {
            /* Once a line has been folded into a still-incomplete block (an
             * open "if" or function "{"), switch to the "> " continuation
             * prompt -- same idea as bash's PS2 -- until it closes. */
            const char *prompt = block_used > 0 || line_continued ? shell_console__continuation_prompt() : NULL;
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
                line_continued = false;
                continue;
            }
            int status = 128 + (int)signal;
            memory__free(line);
            memory__free(block);
            return status;
        }
        if (length < 0) break;
        /* A trailing, unescaped "\" outside single quotes -- see
         * shell_app__line_continuation_length()/shell_app__span_ends_in_single_quote()
         * above -- means "more of this same line is coming", with the
         * backslash itself dropped (matching bash). `line_continued` (still
         * holding *last* iteration's verdict here) says whether the text
         * we're about to append is itself such a continuation's remainder --
         * that's what decides whether it's glued directly onto `block` with
         * no separator, not whether this new fragment happens to end in
         * "\" too. That keeps a whole "a\<NL>b\<NL>c" chain joining as one
         * "abc": the "a\" and "b\" lines each set line_continued for the
         * line after them, and "c" -- itself not a continuation -- still
         * lands with no separator because the line before *it* was one.
         * Inside single quotes a trailing "\" is just a literal character
         * with no special meaning, so it falls through to the normal
         * "keep this line's own text" case exactly like any other
         * unterminated-quote continuation already does. */
        size_t continuation_length = 0;
        bool continues = length > 0 &&
            shell_app__line_continuation_length(line, (size_t)length, &continuation_length) &&
            !shell_app__span_ends_in_single_quote(block, block_used, line, continuation_length);
        size_t append_length = continues ? continuation_length : (size_t)length;
        if (!shell__block_append(block, SHELL__BLOCK_MAX, &block_used, line, append_length, line_continued)) {
            stdio__printf("shell: input too long\n");
            block_used = 0;
            block[0] = '\0';
            line_continued = false;
            continue;
        }
        line_continued = continues;
        if (continues) continue;
        if (!shell_compound__pending(block)) {
            shell_app__interactive_execute(state, block, block_used);
            block_used = 0;
            block[0] = '\0';
        }
    }
    if (!state->exit_requested && block_used > 0) {
        if (shell_compound__pending(block)) {
            stdio__printf("shell: unexpected end of input (unterminated if/function)\n");
        } else {
            shell_app__interactive_execute(state, block, block_used);
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
