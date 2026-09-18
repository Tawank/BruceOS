#include "bnu_app.h"
#include "bnu_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Stream editor: sed. A deliberately small subset of one command per
 * invocation -- no script list, no hold space, no branching, no address
 * ranges -- covering the handful of forms that show up in the overwhelming
 * majority of real one-liners:
 *
 *   s<delim>PATTERN<delim>REPLACEMENT<delim>[g][i]   substitute
 *   [/PATTERN/]d                                      delete matching lines
 *   [/PATTERN/]p                                      print matching lines
 *
 * PATTERN is a POSIX ERE (bnu__regex_compile(), same engine bnu_grep_app.c
 * uses). <delim> after 's' can be any character other than a letter, digit,
 * backslash, or NUL (real sed's own rule) -- handy for `s|/old/|/new/|`
 * paths without escaping every '/'. A delimiter that does need to appear
 * literally inside PATTERN/REPLACEMENT is written escaped ("\<delim>");
 * every other backslash sequence passes through untouched, so it still
 * reaches the regex compiler (PATTERN) or the replacement expander
 * (REPLACEMENT) with its own meaning intact.
 *
 * Content is loaded fully into external memory (capped at BNU_SED_MAX_BYTES,
 * the same limit `grep`/`less` use for the same reason) and processed one
 * line at a time; REPLACEMENT is expanded into a small growable scratch
 * buffer (bnu_sed_buffer_t) reused across lines, since a substitution's
 * output length isn't known ahead of time.
 */

#define BNU_SED_MAX_BYTES (512u * 1024u)
/* Backreferences \1-\9 -- a tenth would need two digits anyway. */
#define BNU_SED_MAX_CAPTURES 9

typedef enum {
    BNU_SED_CMD_SUBSTITUTE,
    BNU_SED_CMD_DELETE,
    BNU_SED_CMD_PRINT,
} bnu_sed_command_t;

typedef struct {
    bnu_sed_command_t command;
    /* Substitute: always true (`regex` is PATTERN). Delete/print: whether an
     * address (`/PATTERN/`) was given at all -- with none, every line is
     * "selected", matching bare "d"/"p" in real sed. */
    bool has_regex;
    regex_t regex;
    bool global; /* s///g */
    /* Owned, NUL-terminated, delimiter-unescaped. Only set for substitute. */
    char *replacement;
} bnu_sed_script_t;

typedef struct {
    const bnu_sed_script_t *script;
    bool suppress; /* -n */
} bnu_sed_options_t;

/* Growable scratch buffer for one expanded substitution result at a time --
 * reused (length reset, capacity kept) across lines rather than reallocated
 * per line. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} bnu_sed_buffer_t;

static bruce_result_t bnu__sed_buffer_append(bnu_sed_buffer_t *buf, const char *data, size_t length) {
    if (length == 0) return BRUCE_OK;
    if (buf->length + length > buf->capacity) {
        size_t capacity = buf->capacity == 0 ? 256 : buf->capacity;
        while (capacity < buf->length + length) capacity *= 2;
        char *grown = memory__realloc(buf->data, capacity);
        if (grown == NULL) return BRUCE_ERR_NO_MEMORY;
        buf->data = grown;
        buf->capacity = capacity;
    }
    memcpy(buf->data + buf->length, data, length);
    buf->length += length;
    return BRUCE_OK;
}

/* Finds the next `delim` in [cursor, end), skipping over every "\X" pair
 * (X anything) as a unit so an escaped delimiter is never mistaken for the
 * end of PATTERN/REPLACEMENT. */
static const char *bnu__sed_find_delim(const char *cursor, const char *end, char delim) {
    while (cursor < end) {
        if (*cursor == '\\' && cursor + 1 < end) {
            cursor += 2;
            continue;
        }
        if (*cursor == delim) return cursor;
        cursor++;
    }
    return NULL;
}

/* Copies [start, end) into a new memory__malloc()'d NUL-terminated buffer,
 * collapsing "\<delim>" into a literal <delim> byte; every other backslash
 * sequence passes through verbatim (PATTERN's own regex escapes, or
 * REPLACEMENT's \1-\9/\&/\\, are for bnu__regex_compile()/bnu__sed_substitute()
 * to interpret, not this function). Caller frees with memory__free(). */
static char *bnu__sed_unescape_delim(const char *start, const char *end, char delim) {
    char *out = memory__malloc((size_t)(end - start) + 1);
    if (out == NULL) return NULL;
    size_t out_length = 0;
    const char *cursor = start;
    while (cursor < end) {
        if (*cursor == '\\' && cursor + 1 < end && cursor[1] == delim) {
            out[out_length++] = delim;
            cursor += 2;
        } else {
            out[out_length++] = *cursor++;
        }
    }
    out[out_length] = '\0';
    return out;
}

static void bnu__sed_script_free(bnu_sed_script_t *script) {
    if (script->has_regex) regfree(&script->regex);
    if (script->replacement != NULL) memory__free(script->replacement);
}

static bruce_result_t
bnu__sed_parse_script(const char *script_text, bnu_sed_script_t *out, char *error, size_t error_capacity) {
    memset(out, 0, sizeof(*out));
    size_t length = strlen(script_text);
    const char *end = script_text + length;

    if (script_text[0] == 's') {
        if (length < 2) {
            snprintf(error, error_capacity, "unterminated 's' command");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        char delim = script_text[1];
        if (delim == '\\' || isalnum((unsigned char)delim)) {
            snprintf(error, error_capacity, "invalid delimiter for 's'");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        const char *pattern_start = script_text + 2;
        const char *pattern_end = bnu__sed_find_delim(pattern_start, end, delim);
        const char *replacement_start = pattern_end != NULL ? pattern_end + 1 : NULL;
        const char *replacement_end =
            replacement_start != NULL ? bnu__sed_find_delim(replacement_start, end, delim) : NULL;
        if (replacement_end == NULL) {
            snprintf(error, error_capacity, "unterminated 's' command");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        bool ignore_case = false;
        for (const char *f = replacement_end + 1; f < end; ++f) {
            if (*f == 'g') out->global = true;
            else if (*f == 'i' || *f == 'I') ignore_case = true;
            else {
                snprintf(error, error_capacity, "unknown option to 's': %c", *f);
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
        }

        char *pattern_text = bnu__sed_unescape_delim(pattern_start, pattern_end, delim);
        if (pattern_text == NULL) return BRUCE_ERR_NO_MEMORY;
        bruce_result_t compile_result =
            bnu__regex_compile(pattern_text, ignore_case, &out->regex, error, error_capacity);
        memory__free(pattern_text);
        if (compile_result != BRUCE_OK) return compile_result;
        out->has_regex = true;

        out->replacement = bnu__sed_unescape_delim(replacement_start, replacement_end, delim);
        if (out->replacement == NULL) {
            regfree(&out->regex);
            return BRUCE_ERR_NO_MEMORY;
        }
        out->command = BNU_SED_CMD_SUBSTITUTE;
        return BRUCE_OK;
    }

    if (script_text[0] == '/') {
        const char *pattern_start = script_text + 1;
        const char *pattern_end = bnu__sed_find_delim(pattern_start, end, '/');
        if (pattern_end == NULL || pattern_end + 2 != end || (pattern_end[1] != 'd' && pattern_end[1] != 'p')) {
            snprintf(error, error_capacity, "expected /PATTERN/d or /PATTERN/p");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        char *pattern_text = bnu__sed_unescape_delim(pattern_start, pattern_end, '/');
        if (pattern_text == NULL) return BRUCE_ERR_NO_MEMORY;
        bruce_result_t compile_result = bnu__regex_compile(pattern_text, false, &out->regex, error, error_capacity);
        memory__free(pattern_text);
        if (compile_result != BRUCE_OK) return compile_result;
        out->has_regex = true;
        out->command = pattern_end[1] == 'd' ? BNU_SED_CMD_DELETE : BNU_SED_CMD_PRINT;
        return BRUCE_OK;
    }

    if (strcmp(script_text, "d") == 0) {
        out->command = BNU_SED_CMD_DELETE;
        return BRUCE_OK;
    }
    if (strcmp(script_text, "p") == 0) {
        out->command = BNU_SED_CMD_PRINT;
        return BRUCE_OK;
    }

    snprintf(error, error_capacity, "unrecognized command");
    return BRUCE_ERR_INVALID_ARGUMENT;
}

/* Expands `script->replacement` for one match into `out`: '&' is the whole
 * match, "\1".."\9" a capture group (empty if that group didn't participate,
 * or if the pattern has fewer groups than the digit -- same as GNU sed),
 * "\&"/"\\" their literal characters, and any other "\X" just X (the
 * backslash dropped). `matches`/`nmatch` are the regexec() results for the
 * current match, offsets relative to `line`. */
static bruce_result_t bnu__sed_expand_replacement(
    const bnu_sed_script_t *script, const char *line, const regmatch_t *matches, size_t nmatch, bnu_sed_buffer_t *out
) {
    bruce_result_t result = BRUCE_OK;
    const char *r = script->replacement;
    while (*r != '\0' && result == BRUCE_OK) {
        if (*r == '&') {
            result = bnu__sed_buffer_append(
                out, line + matches[0].rm_so, (size_t)(matches[0].rm_eo - matches[0].rm_so)
            );
            r++;
        } else if (*r == '\\' && r[1] != '\0') {
            char next = r[1];
            if (next >= '1' && next <= '9') {
                size_t group = (size_t)(next - '0');
                if (group < nmatch && matches[group].rm_so >= 0) {
                    result = bnu__sed_buffer_append(
                        out, line + matches[group].rm_so, (size_t)(matches[group].rm_eo - matches[group].rm_so)
                    );
                }
            } else {
                result = bnu__sed_buffer_append(out, &next, 1);
            }
            r += 2;
        } else {
            result = bnu__sed_buffer_append(out, r, 1);
            r++;
        }
    }
    return result;
}

/* Runs script->regex against `line[0..length)` (via REG_STARTEND, same as
 * bnu__regex_matches() -- see its own doc comment), replacing every match
 * (script->global) or just the first, and appends the result -- substituted
 * or, on no match at all, the untouched original -- to `out` (reset by the
 * caller first). An empty match advances one byte before searching again,
 * the usual guard against looping forever on it. */
static bruce_result_t bnu__sed_substitute(
    const bnu_sed_script_t *script, const char *line, size_t length, bnu_sed_buffer_t *out
) {
    size_t nmatch = script->regex.re_nsub + 1;
    if (nmatch > BNU_SED_MAX_CAPTURES + 1) nmatch = BNU_SED_MAX_CAPTURES + 1;
    regmatch_t matches[BNU_SED_MAX_CAPTURES + 1];

    size_t search_start = 0;
    bruce_result_t result = BRUCE_OK;
    for (;;) {
        matches[0].rm_so = (regoff_t)search_start;
        matches[0].rm_eo = (regoff_t)length;
        if (regexec(&script->regex, line, nmatch, matches, REG_STARTEND) != 0) break;
        size_t match_start = (size_t)matches[0].rm_so;
        size_t match_end = (size_t)matches[0].rm_eo;

        result = bnu__sed_buffer_append(out, line + search_start, match_start - search_start);
        if (result == BRUCE_OK) result = bnu__sed_expand_replacement(script, line, matches, nmatch, out);
        if (result != BRUCE_OK) return result;

        if (match_start == match_end) {
            if (match_end < length) {
                result = bnu__sed_buffer_append(out, line + match_end, 1);
                if (result != BRUCE_OK) return result;
            }
            search_start = match_end + 1;
        } else {
            search_start = match_end;
        }
        if (!script->global || search_start > length) break;
    }
    if (search_start < length) result = bnu__sed_buffer_append(out, line + search_start, length - search_start);
    return result;
}

static bruce_result_t bnu__sed_write_line(const char *text, size_t length, bool trailing_newline) {
    bruce_result_t result = length > 0 ? stdio__write(text, length) : BRUCE_OK;
    if (result == BRUCE_OK && trailing_newline) result = stdio__write("\n", 1);
    return result;
}

static bruce_result_t bnu__sed_process_buffer(const bnu_sed_options_t *opt, const char *data, size_t length) {
    bnu_sed_buffer_t line_out = {0};
    bruce_result_t result = BRUCE_OK;
    size_t pos = 0;
    while (pos < length && result == BRUCE_OK) {
        size_t line_start = pos;
        while (pos < length && data[pos] != '\n') pos++;
        size_t line_length = pos - line_start;
        if (line_length > 0 && data[line_start + line_length - 1] == '\r') line_length--;
        const char *line_text = data + line_start;
        bool has_newline = pos < length;
        if (has_newline) pos++; /* skip the '\n' */

        bool selected =
            !opt->script->has_regex || bnu__regex_matches(&opt->script->regex, line_text, line_length);

        switch (opt->script->command) {
            case BNU_SED_CMD_SUBSTITUTE:
                line_out.length = 0;
                result = bnu__sed_substitute(opt->script, line_text, line_length, &line_out);
                if (result == BRUCE_OK && !opt->suppress) {
                    result = bnu__sed_write_line(line_out.data, line_out.length, has_newline);
                }
                break;
            case BNU_SED_CMD_DELETE:
                if (!selected && !opt->suppress) result = bnu__sed_write_line(line_text, line_length, has_newline);
                break;
            case BNU_SED_CMD_PRINT:
                if (selected) result = bnu__sed_write_line(line_text, line_length, has_newline);
                if (result == BRUCE_OK && !opt->suppress) {
                    result = bnu__sed_write_line(line_text, line_length, has_newline);
                }
                break;
        }
    }
    if (line_out.data != NULL) memory__free(line_out.data);
    return result;
}

int bnu_sed_app_main(int argc, char **argv) {
    ArgParser *parser =
        bnu__new_parser("Stream editor: applies one editing command to each line of files or stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "n");
    ap_set_opt_help(parser, "n", "Suppress automatic printing; only an explicit 'p' command emits a line");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_required_arg(
        parser, "script", "s<delim>PATTERN<delim>REPLACEMENT<delim>[gi], [/PATTERN/]d, or [/PATTERN/]p"
    );
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *script_text = ap_get_arg_at_index(parser, 0);
    char error[128];
    bnu_sed_script_t script;
    bruce_result_t result = bnu__sed_parse_script(script_text, &script, error, sizeof(error));
    if (result != BRUCE_OK) {
        stdio__printf("sed: %s: %s\n", script_text, error);
        ap_free(parser);
        return result;
    }
    bnu_sed_options_t opt = {.script = &script, .suppress = ap_found(parser, "n")};

    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");
    int total = ap_count_args(parser);
    int file_count = total - 1;

    if (file_count == 0) {
        char *end = NULL;
        unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
        bool valid_stdin_size = stdin_size_arg != NULL && stdin_size_arg[0] != '\0' && end != NULL &&
                                 *end == '\0' && parsed_stdin_size <= BNU_SED_MAX_BYTES;
        if (!valid_stdin_size) {
            stdio__printf("sed: missing filename\n");
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            const void *data = NULL;
            size_t length = 0;
            result = bnu__load_stdin((size_t)parsed_stdin_size, &data, &length);
            if (result != BRUCE_OK) {
                stdio__printf("sed: (standard input): %s\n", result__to_string(result));
            } else {
                result = bnu__sed_process_buffer(&opt, data != NULL ? data : "", length);
                if (data != NULL) (void)memory__external_free(data);
            }
        }
    } else {
        for (int i = 1; i <= file_count && result == BRUCE_OK; ++i) {
            char path[BRUCE_STORAGE_PATH_MAX];
            if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
                result = BRUCE_ERR_INVALID_PATH;
                break;
            }
            const void *data = NULL;
            size_t length = 0;
            result = bnu__load_path(path, BNU_SED_MAX_BYTES, &data, &length);
            if (result != BRUCE_OK) {
                stdio__printf("sed: %s: %s\n", path, result__to_string(result));
                break;
            }
            result = bnu__sed_process_buffer(&opt, data != NULL ? data : "", length);
            if (data != NULL) (void)memory__external_free(data);
        }
    }

    bnu__sed_script_free(&script);
    ap_free(parser);
    return result;
}
