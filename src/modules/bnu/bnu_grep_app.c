#include "bnu_app.h"
#include "bnu_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h> // IWYU pragma: keep (SEEK_SET/SEEK_END)
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Text search command: grep.
 *
 * Basic substring search only -- no regular expressions. Content is loaded
 * fully into external memory (capped at BNU_GREP_MAX_BYTES, the same limit
 * `less` uses for the same reason) and read back through a read-only map;
 * context lines (-A/-B/-C) then just reference spans of that buffer
 * directly instead of being copied into a separate line buffer.
 */

#define BNU_GREP_MAX_BYTES (512u * 1024u)
#define BNU_GREP_MAX_CONTEXT 1000u

typedef struct {
    const char *text;
    size_t length;
    uint32_t number;
} bnu_grep_line_t;

typedef struct {
    const char *pattern;
    size_t pattern_length;
    bool ignore_case;
    bool invert;
    bool show_numbers;
    bool show_filename;
    bool count_only;
    bool list_only;
    bool silent;
    uint32_t before;
    uint32_t after;
} bnu_grep_options_t;

/* Per-source (per-file/stdin) working state; `ring` is allocated once by the
 * caller and reused across files, but the rest resets for each source. */
typedef struct {
    bnu_grep_line_t *ring;
    uint32_t ring_count;
    uint32_t ring_start;
    uint32_t remaining_after;
    uint32_t last_printed_line;
    uint32_t match_count;
} bnu_grep_run_t;

static bruce_result_t bnu__grep_load_path(const char *path, bruce_memory_object_t *object, size_t *out_length) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK && size > BNU_GREP_MAX_BYTES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);

    bruce_memory_object_t obj = {0};
    if (result == BRUCE_OK && size > 0) result = memory__external_alloc((size_t)size, &obj);
    size_t offset = 0;
    unsigned char chunk[256];
    while (result == BRUCE_OK && offset < (size_t)size) {
        size_t read_size = 0;
        result = storage__read(file, chunk, sizeof(chunk), &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) result = memory__external_write(&obj, offset, chunk, read_size);
        offset += read_size;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        if (obj.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&obj);
        return result;
    }
    *object = obj;
    *out_length = offset;
    return BRUCE_OK;
}

static bruce_result_t bnu__grep_load_stdin(size_t size, bruce_memory_object_t *object, size_t *out_length) {
    bruce_memory_object_t obj = {0};
    bruce_result_t result = size > 0 ? memory__external_alloc(size, &obj) : BRUCE_OK;
    size_t offset = 0;
    unsigned char chunk[256];
    while (result == BRUCE_OK && offset < size) {
        size_t want = size - offset > sizeof(chunk) ? sizeof(chunk) : size - offset;
        size_t read_size = 0;
        result = stdio__read(chunk, want, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) result = memory__external_write(&obj, offset, chunk, read_size);
        offset += read_size;
    }
    if (result != BRUCE_OK) {
        if (obj.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&obj);
        return result;
    }
    *object = obj;
    *out_length = offset;
    return BRUCE_OK;
}

static bool bnu__grep_contains(
    const char *text, size_t text_length, const char *pattern, size_t pattern_length, bool ignore_case
) {
    if (pattern_length == 0) return true;
    if (pattern_length > text_length) return false;
    for (size_t start = 0; start + pattern_length <= text_length; ++start) {
        size_t i = 0;
        for (; i < pattern_length; ++i) {
            unsigned char a = (unsigned char)text[start + i];
            unsigned char b = (unsigned char)pattern[i];
            if (ignore_case) {
                a = (unsigned char)tolower(a);
                b = (unsigned char)tolower(b);
            }
            if (a != b) break;
        }
        if (i == pattern_length) return true;
    }
    return false;
}

static void bnu__grep_print_line(
    const bnu_grep_options_t *opt, const char *filename, uint32_t number, char separator, const char *text,
    size_t length
) {
    if (opt->show_filename) stdio__printf("%s%c", filename, separator);
    if (opt->show_numbers) stdio__printf("%u%c", (unsigned)number, separator);
    if (length > 0) (void)stdio__write(text, length);
    (void)stdio__write("\n", 1);
}

/* Records the most recent `before` lines seen so far, whether or not they
 * matched, so a later match can pull its leading context back out. */
static void bnu__grep_push_context(
    const bnu_grep_options_t *opt, bnu_grep_run_t *run, const char *text, size_t length, uint32_t number
) {
    if (opt->before == 0) return;
    bnu_grep_line_t entry = {.text = text, .length = length, .number = number};
    if (run->ring_count < opt->before) {
        run->ring[(run->ring_start + run->ring_count) % opt->before] = entry;
        run->ring_count++;
    } else {
        run->ring[run->ring_start] = entry;
        run->ring_start = (run->ring_start + 1) % opt->before;
    }
}

/* Prints buffered context lines that come after the last line already
 * printed, with a "--" group separator when they don't immediately follow
 * it (matching real grep's behaviour for non-adjacent match groups). */
static void bnu__grep_flush_context(const bnu_grep_options_t *opt, bnu_grep_run_t *run, const char *filename) {
    if (opt->before == 0 || run->ring_count == 0) return;
    bool need_separator = run->last_printed_line != 0;
    bool printed = false;
    for (uint32_t i = 0; i < run->ring_count; ++i) {
        bnu_grep_line_t *entry = &run->ring[(run->ring_start + i) % opt->before];
        if (entry->number <= run->last_printed_line) continue;
        if (!printed && need_separator && entry->number != run->last_printed_line + 1) {
            (void)stdio__write("--\n", 3);
        }
        printed = true;
        bnu__grep_print_line(opt, filename, entry->number, '-', entry->text, entry->length);
        run->last_printed_line = entry->number;
    }
}

static void bnu__grep_process_buffer(
    const bnu_grep_options_t *opt, const char *filename, const char *data, size_t length, bnu_grep_run_t *run
) {
    bool body_output = !opt->silent && !opt->count_only && !opt->list_only;
    size_t pos = 0;
    uint32_t number = 0;
    while (pos < length) {
        size_t line_start = pos;
        while (pos < length && data[pos] != '\n') pos++;
        size_t line_length = pos - line_start;
        if (line_length > 0 && data[line_start + line_length - 1] == '\r') line_length--;
        number++;
        const char *line_text = data + line_start;

        bool matches =
            bnu__grep_contains(line_text, line_length, opt->pattern, opt->pattern_length, opt->ignore_case);
        if (opt->invert) matches = !matches;

        if (matches) {
            run->match_count++;
            if (body_output) {
                bnu__grep_flush_context(opt, run, filename);
                bnu__grep_print_line(opt, filename, number, ':', line_text, line_length);
                run->last_printed_line = number;
            }
            run->remaining_after = opt->after;
        } else if (run->remaining_after > 0) {
            if (body_output) {
                bnu__grep_print_line(opt, filename, number, '-', line_text, line_length);
                run->last_printed_line = number;
            }
            run->remaining_after--;
        }
        bnu__grep_push_context(opt, run, line_text, line_length, number);

        if (pos < length) pos++; /* skip the '\n' */
    }
}

static bruce_result_t bnu__grep_run_source(
    const bnu_grep_options_t *opt, const char *filename, bruce_memory_object_t *object, size_t length,
    bnu_grep_line_t *ring, bool *out_matched
) {
    const void *data = NULL;
    if (length > 0 && memory__external_map(object, &data) != BRUCE_OK) return BRUCE_ERR_IO;

    bnu_grep_run_t run = {.ring = ring};
    bnu__grep_process_buffer(opt, filename, data != NULL ? data : "", length, &run);

    if (!opt->silent) {
        if (opt->count_only) {
            if (opt->show_filename) stdio__printf("%s:%u\n", filename, (unsigned)run.match_count);
            else stdio__printf("%u\n", (unsigned)run.match_count);
        } else if (opt->list_only && run.match_count > 0) {
            stdio__printf("%s\n", filename);
        }
    }
    *out_matched = run.match_count > 0;
    return BRUCE_OK;
}

int bnu_grep_app_main(int argc, char **argv) {
    ArgParser *parser =
        bnu__new_parser("Search for a literal substring in files or stdin (no regular expressions).");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "i");
    ap_set_opt_help(parser, "i", "Ignore case when matching");
    ap_add_flag(parser, "v");
    ap_set_opt_help(parser, "v", "Select non-matching lines instead");
    ap_add_flag(parser, "n");
    ap_set_opt_help(parser, "n", "Prefix each output line with its line number");
    ap_add_flag(parser, "c");
    ap_set_opt_help(parser, "c", "Print only a count of matching lines per file");
    ap_add_flag(parser, "l");
    ap_set_opt_help(parser, "l", "Print only the names of files containing a match");
    ap_add_flag(parser, "q");
    ap_set_opt_help(parser, "q", "Suppress all output; only the exit status reports a match");
    ap_add_int_opt(parser, "A", 0);
    ap_set_opt_help(parser, "A", "Print NUM lines of context after each match");
    ap_add_int_opt(parser, "B", 0);
    ap_set_opt_help(parser, "B", "Print NUM lines of context before each match");
    ap_add_int_opt(parser, "C", 0);
    ap_set_opt_help(parser, "C", "Print NUM lines of context before and after each match (like -A NUM -B NUM)");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_required_arg(parser, "pattern", "Literal substring to search for (no regex)");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bnu_grep_options_t opt = {
        .ignore_case = ap_found(parser, "i"),
        .invert = ap_found(parser, "v"),
        .show_numbers = ap_found(parser, "n"),
        .count_only = ap_found(parser, "c"),
        .list_only = ap_found(parser, "l"),
        .silent = ap_found(parser, "q"),
    };

    bool a_found = ap_found(parser, "A");
    bool b_found = ap_found(parser, "B");
    bool c_found = ap_found(parser, "C");
    int before = b_found ? ap_get_int_value(parser, "B") : (c_found ? ap_get_int_value(parser, "C") : 0);
    int after = a_found ? ap_get_int_value(parser, "A") : (c_found ? ap_get_int_value(parser, "C") : 0);
    if (before < 0 || after < 0 || before > (int)BNU_GREP_MAX_CONTEXT || after > (int)BNU_GREP_MAX_CONTEXT) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    opt.before = (uint32_t)before;
    opt.after = (uint32_t)after;

    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");
    int total = ap_count_args(parser);
    opt.pattern = ap_get_arg_at_index(parser, 0);
    opt.pattern_length = opt.pattern != NULL ? strlen(opt.pattern) : 0;
    int file_count = total - 1;
    opt.show_filename = file_count > 1;

    bnu_grep_line_t *ring = NULL;
    if (opt.before > 0) {
        ring = memory__malloc(opt.before * sizeof(*ring));
        if (ring == NULL) {
            ap_free(parser);
            return BRUCE_ERR_NO_MEMORY;
        }
    }

    bool matched_any = false;
    bruce_result_t result = BRUCE_OK;

    if (file_count == 0) {
        char *end = NULL;
        unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
        bool valid_stdin_size = stdin_size_arg != NULL && stdin_size_arg[0] != '\0' && end != NULL &&
                                 *end == '\0' && parsed_stdin_size <= BNU_GREP_MAX_BYTES;
        if (!valid_stdin_size) {
            stdio__printf("grep: missing filename\n");
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            bruce_memory_object_t object = {0};
            size_t length = 0;
            result = bnu__grep_load_stdin((size_t)parsed_stdin_size, &object, &length);
            if (result != BRUCE_OK) {
                stdio__printf("grep: (standard input): %s\n", result__to_string(result));
            } else {
                result = bnu__grep_run_source(&opt, "(standard input)", &object, length, ring, &matched_any);
                if (object.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&object);
            }
        }
    } else {
        for (int i = 1; i <= file_count && result == BRUCE_OK; ++i) {
            char path[BRUCE_STORAGE_PATH_MAX];
            if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
                result = BRUCE_ERR_INVALID_PATH;
                break;
            }
            bruce_memory_object_t object = {0};
            size_t length = 0;
            result = bnu__grep_load_path(path, &object, &length);
            if (result != BRUCE_OK) {
                stdio__printf("grep: %s: %s\n", path, result__to_string(result));
                break;
            }
            bool file_matched = false;
            result = bnu__grep_run_source(&opt, path, &object, length, ring, &file_matched);
            if (file_matched) matched_any = true;
            if (object.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&object);
        }
    }

    if (ring != NULL) memory__free(ring);
    ap_free(parser);
    if (result != BRUCE_OK) return result;
    return matched_any ? BRUCE_OK : BRUCE_ERR_NOT_FOUND;
}
