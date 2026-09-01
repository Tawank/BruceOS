#include "bnu_app.h"
#include "bnu_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/* Text-processing commands: tr, uniq, cut, sort, seq, tee, rev. */

/* uniq/cut/sort/rev all need random access to whatever they're processing,
 * so (like grep and head/tail) they load it fully via bnu__load_path()/
 * bnu__load_stdin() rather than streaming it. tr and tee never need to look
 * backward or forward past the current byte, so they stream in fixed-size
 * chunks instead (see bnu_wc_app.c) and carry no size cap. seq never reads
 * anything at all. */
#define BNU_TEXT_MAX_BYTES (512u * 1024u)

/* Advances *cursor to the next '\n'-terminated line in `data` (trimming a
 * trailing '\r', matching grep's CRLF handling), or returns false once
 * *cursor has reached `length`. A final line with no trailing '\n' is still
 * returned once, matching how head/tail/grep already treat one. */
static bool bnu__text_next_line(
    const char *data, size_t length, size_t *cursor, size_t *out_start, size_t *out_length
) {
    if (*cursor >= length) return false;
    size_t start = *cursor;
    size_t end = start;
    while (end < length && data[end] != '\n') end++;
    size_t line_length = end - start;
    if (line_length > 0 && data[start + line_length - 1] == '\r') line_length--;
    *out_start = start;
    *out_length = line_length;
    *cursor = end < length ? end + 1 : end;
    return true;
}

/* Shared by every uniq/cut/sort/rev command below to resolve their one
 * optional file operand, or fall back to --stdin-size the same way
 * wc/grep/head/tail do. `command` names the caller for its error messages,
 * which this prints itself (rather than handing the caller a `path` that,
 * on a resolve failure, bnu__resolve_path may never have written to). */
static bruce_result_t bnu__text_load_source(
    const char *command, const char *file_arg, const char *stdin_size_arg, const void **out_data,
    size_t *out_length
) {
    if (file_arg != NULL) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(file_arg, path)) return BRUCE_ERR_INVALID_PATH;
        bruce_result_t result = bnu__load_path(path, BNU_TEXT_MAX_BYTES, out_data, out_length);
        if (result != BRUCE_OK) stdio__printf("%s: %s: error %d\n", command, path, result);
        return result;
    }

    char *end = NULL;
    unsigned long parsed = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
    bool valid = stdin_size_arg != NULL && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0' &&
                 parsed <= BNU_TEXT_MAX_BYTES;
    if (!valid) {
        stdio__printf("%s: missing filename\n", command);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = bnu__load_stdin((size_t)parsed, out_data, out_length);
    if (result != BRUCE_OK) stdio__printf("%s: (standard input): error %d\n", command, result);
    return result;
}

/* ---- uniq --------------------------------------------------------------- */

static bool bnu__uniq_lines_equal(
    const char *a, size_t a_length, const char *b, size_t b_length, bool ignore_case
) {
    if (a_length != b_length) return false;
    for (size_t i = 0; i < a_length; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ignore_case) {
            ca = (unsigned char)tolower(ca);
            cb = (unsigned char)tolower(cb);
        }
        if (ca != cb) return false;
    }
    return true;
}

static void bnu__uniq_flush(
    bool show_count, bool dups_only, bool uniques_only, bool have_group, const char *text, size_t length,
    size_t count
) {
    if (!have_group) return;
    if (dups_only && count < 2) return;
    if (uniques_only && count > 1) return;
    if (show_count) stdio__printf("%7u ", (unsigned)count);
    if (length > 0) (void)stdio__write(text, length);
    (void)stdio__write("\n", 1);
}

int bnu_uniq_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Filter out repeated adjacent lines from a file or stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "c");
    ap_set_opt_help(parser, "c", "Prefix each line with its number of occurrences");
    ap_add_flag(parser, "d");
    ap_set_opt_help(parser, "d", "Print only lines that were repeated");
    ap_add_flag(parser, "u");
    ap_set_opt_help(parser, "u", "Print only lines that were not repeated");
    ap_add_flag(parser, "i");
    ap_set_opt_help(parser, "i", "Ignore case when comparing lines");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to read (defaults to stdin)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool show_count = ap_found(parser, "c");
    bool dups_only = ap_found(parser, "d");
    bool uniques_only = ap_found(parser, "u");
    bool ignore_case = ap_found(parser, "i");
    const char *file_arg = ap_get_arg(parser, "file");
    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");
    ap_free(parser);

    const void *data = NULL;
    size_t length = 0;
    bruce_result_t result = bnu__text_load_source("uniq", file_arg, stdin_size_arg, &data, &length);
    if (result != BRUCE_OK) return result;

    const char *text = (const char *)data;
    size_t cursor = 0, group_start = 0, group_length = 0, group_count = 0;
    bool have_group = false;
    size_t line_start = 0, line_length = 0;
    while (bnu__text_next_line(text, length, &cursor, &line_start, &line_length)) {
        bool same_as_group =
            have_group && bnu__uniq_lines_equal(
                              text + group_start, group_length, text + line_start, line_length, ignore_case
                          );
        if (same_as_group) {
            group_count++;
            continue;
        }
        bnu__uniq_flush(
            show_count, dups_only, uniques_only, have_group, text + group_start, group_length, group_count
        );
        group_start = line_start;
        group_length = line_length;
        group_count = 1;
        have_group = true;
    }
    bnu__uniq_flush(
        show_count, dups_only, uniques_only, have_group, text + group_start, group_length, group_count
    );

    if (data != NULL) (void)memory__external_free(data);
    return BRUCE_OK;
}

/* ---- cut ------------------------------------------------------------------ */

#define BNU_CUT_MAX_RANGES 32

typedef struct {
    unsigned long start;
    unsigned long end; /* 0 means unbounded ("N-") */
} bnu_cut_range_t;

/* Parses a comma-separated list of 1-based indices/ranges ("1,3-5,8-") into
 * `ranges`. Returns false on a malformed list or more ranges than fit. */
static bool bnu__cut_parse_list(
    const char *list, bnu_cut_range_t *ranges, size_t max_ranges, size_t *out_count
) {
    size_t count = 0;
    const char *cursor = list;
    while (*cursor != '\0') {
        if (count >= max_ranges) return false;
        char *end = NULL;
        unsigned long start = 1, stop = 0;
        bool has_stop = false;
        if (*cursor != '-') {
            start = strtoul(cursor, &end, 10);
            if (end == cursor || start == 0) return false;
            cursor = end;
        }
        if (*cursor == '-') {
            cursor++;
            if (*cursor != '\0' && *cursor != ',') {
                stop = strtoul(cursor, &end, 10);
                if (end == cursor) return false;
                has_stop = true;
                cursor = end;
            }
        } else {
            stop = start;
            has_stop = true;
        }
        ranges[count].start = start;
        ranges[count].end = has_stop ? stop : 0;
        count++;
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == '\0') break;
        return false;
    }
    *out_count = count;
    return count > 0;
}

static bool bnu__cut_selected(const bnu_cut_range_t *ranges, size_t count, unsigned long index) {
    for (size_t i = 0; i < count; ++i) {
        if (index >= ranges[i].start && (ranges[i].end == 0 || index <= ranges[i].end)) return true;
    }
    return false;
}

int bnu_cut_app_main(int argc, char **argv) {
    ArgParser *parser =
        bnu__new_parser("Extract selected fields or characters from each line of a file or stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "f", NULL);
    ap_set_opt_help(parser, "f", "Field list to keep, e.g. 1,3-5 (columns separated by -d)");
    ap_add_str_opt(parser, "c", NULL);
    ap_set_opt_help(parser, "c", "Character list to keep, e.g. 1-4,8");
    ap_add_str_opt(parser, "d", NULL);
    ap_set_opt_help(parser, "d", "Field separator for -f (default: tab)");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to read (defaults to stdin)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *field_list = ap_get_str_value(parser, "f");
    const char *char_list = ap_get_str_value(parser, "c");
    const char *delim_arg = ap_get_str_value(parser, "d");
    char delim = delim_arg != NULL && delim_arg[0] != '\0' ? delim_arg[0] : '\t';
    const char *file_arg = ap_get_arg(parser, "file");
    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");

    bool by_chars = char_list != NULL;
    const char *list = by_chars ? char_list : field_list;
    if (list == NULL) {
        stdio__printf("cut: specify a list with -f (fields) or -c (characters)\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bnu_cut_range_t ranges[BNU_CUT_MAX_RANGES];
    size_t range_count = 0;
    if (!bnu__cut_parse_list(list, ranges, BNU_CUT_MAX_RANGES, &range_count)) {
        stdio__printf("cut: invalid list -- '%s'\n", list);
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    const void *data = NULL;
    size_t length = 0;
    bruce_result_t result = bnu__text_load_source("cut", file_arg, stdin_size_arg, &data, &length);
    ap_free(parser);
    if (result != BRUCE_OK) return result;

    const char *text = (const char *)data;
    size_t cursor = 0, line_start = 0, line_length = 0;
    while (bnu__text_next_line(text, length, &cursor, &line_start, &line_length)) {
        if (by_chars) {
            for (size_t i = 0; i < line_length; ++i) {
                if (bnu__cut_selected(ranges, range_count, (unsigned long)(i + 1))) {
                    (void)stdio__write(text + line_start + i, 1);
                }
            }
        } else {
            size_t field_start = 0;
            unsigned long field_index = 1;
            bool printed_any = false;
            for (size_t i = 0; i <= line_length; ++i) {
                bool at_delim = i == line_length || text[line_start + i] == delim;
                if (!at_delim) continue;
                if (bnu__cut_selected(ranges, range_count, field_index)) {
                    if (printed_any) (void)stdio__write(&delim, 1);
                    if (i > field_start) (void)stdio__write(text + line_start + field_start, i - field_start);
                    printed_any = true;
                }
                field_start = i + 1;
                field_index++;
            }
        }
        (void)stdio__write("\n", 1);
    }

    if (data != NULL) (void)memory__external_free(data);
    return BRUCE_OK;
}

/* ---- sort ------------------------------------------------------------------ */

typedef struct {
    size_t start;
    size_t length;
} bnu_sort_line_t;

/* qsort() takes no user context, so (like bnu_sys_app.c's/apps_app.c's own
 * comparators) the buffer and options live in file statics for the
 * duration of one sort_app_main() call; bnu commands run to completion
 * before another one starts, so this isn't reentered. */
static const char *s_sort_text;
static bool s_sort_numeric;
static bool s_sort_reverse;

static double bnu__sort_numeric_key(const char *text, size_t length) {
    char buffer[64];
    size_t copy_length = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
    memcpy(buffer, text, copy_length);
    buffer[copy_length] = '\0';
    char *end = NULL;
    double value = strtod(buffer, &end);
    return end != buffer ? value : 0.0;
}

static int bnu__sort_compare_text(const char *a, size_t a_length, const char *b, size_t b_length) {
    size_t min_length = a_length < b_length ? a_length : b_length;
    int order = min_length > 0 ? memcmp(a, b, min_length) : 0;
    if (order != 0) return order;
    if (a_length != b_length) return a_length < b_length ? -1 : 1;
    return 0;
}

static int bnu__sort_compare(const void *left, const void *right) {
    const bnu_sort_line_t *a = left;
    const bnu_sort_line_t *b = right;
    int order;
    if (s_sort_numeric) {
        double a_value = bnu__sort_numeric_key(s_sort_text + a->start, a->length);
        double b_value = bnu__sort_numeric_key(s_sort_text + b->start, b->length);
        order = a_value < b_value ? -1 : (a_value > b_value ? 1 : 0);
    } else {
        order = bnu__sort_compare_text(s_sort_text + a->start, a->length, s_sort_text + b->start, b->length);
    }
    return s_sort_reverse ? -order : order;
}

int bnu_sort_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Sort the lines of a file or stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "r");
    ap_set_opt_help(parser, "r", "Reverse the sort order");
    ap_add_flag(parser, "n");
    ap_set_opt_help(parser, "n", "Compare by each line's leading numeric value");
    ap_add_flag(parser, "u");
    ap_set_opt_help(parser, "u", "Discard lines identical to the one before them, once sorted");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to read (defaults to stdin)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool reverse = ap_found(parser, "r");
    bool numeric = ap_found(parser, "n");
    bool unique = ap_found(parser, "u");
    const char *file_arg = ap_get_arg(parser, "file");
    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");

    const void *data = NULL;
    size_t length = 0;
    bruce_result_t result = bnu__text_load_source("sort", file_arg, stdin_size_arg, &data, &length);
    ap_free(parser);
    if (result != BRUCE_OK) return result;

    const char *text = (const char *)data;
    size_t line_count = 0;
    {
        size_t cursor = 0, line_start = 0, line_length = 0;
        while (bnu__text_next_line(text, length, &cursor, &line_start, &line_length)) line_count++;
    }

    if (line_count > 0) {
        bnu_sort_line_t *lines = memory__malloc(line_count * sizeof(*lines));
        if (lines == NULL) {
            result = BRUCE_ERR_NO_MEMORY;
        } else {
            size_t cursor = 0, line_start = 0, line_length = 0, i = 0;
            while (bnu__text_next_line(text, length, &cursor, &line_start, &line_length)) {
                lines[i].start = line_start;
                lines[i].length = line_length;
                i++;
            }

            s_sort_text = text;
            s_sort_numeric = numeric;
            s_sort_reverse = reverse;
            qsort(lines, line_count, sizeof(*lines), bnu__sort_compare);

            bool have_prev = false;
            size_t prev_start = 0, prev_length = 0;
            for (i = 0; i < line_count; ++i) {
                bool same_as_prev =
                    have_prev &&
                    bnu__sort_compare_text(
                        text + prev_start, prev_length, text + lines[i].start, lines[i].length
                    ) == 0;
                if (unique && same_as_prev) continue;
                if (lines[i].length > 0) (void)stdio__write(text + lines[i].start, lines[i].length);
                (void)stdio__write("\n", 1);
                prev_start = lines[i].start;
                prev_length = lines[i].length;
                have_prev = true;
            }
            memory__free(lines);
        }
    }

    if (data != NULL) (void)memory__external_free(data);
    return result;
}

/* ---- rev -------------------------------------------------------------------- */

int bnu_rev_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Reverse the characters of each line in a file or stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to read (defaults to stdin)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *file_arg = ap_get_arg(parser, "file");
    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");

    const void *data = NULL;
    size_t length = 0;
    bruce_result_t result = bnu__text_load_source("rev", file_arg, stdin_size_arg, &data, &length);
    ap_free(parser);
    if (result != BRUCE_OK) return result;

    const char *text = (const char *)data;
    char *reversed = NULL;
    size_t reversed_capacity = 0;
    size_t cursor = 0, line_start = 0, line_length = 0;
    while (bnu__text_next_line(text, length, &cursor, &line_start, &line_length)) {
        if (line_length > reversed_capacity) {
            memory__free(reversed);
            reversed = memory__malloc(line_length);
            reversed_capacity = reversed != NULL ? line_length : 0;
            if (reversed == NULL) {
                result = BRUCE_ERR_NO_MEMORY;
                break;
            }
        }
        for (size_t i = 0; i < line_length; ++i) reversed[i] = text[line_start + line_length - 1 - i];
        if (line_length > 0) (void)stdio__write(reversed, line_length);
        (void)stdio__write("\n", 1);
    }
    memory__free(reversed);

    if (data != NULL) (void)memory__external_free(data);
    return result;
}

/* ---- tr --------------------------------------------------------------------- */

#define BNU_TR_SET_MAX 256

/* Expands a tr set spec ("a-z", "\n\t", literal chars) into `out`, one byte
 * per matched character. Range ends are taken literally (no backslash
 * escapes there) and complement ("-c") / POSIX classes ("[:upper:]") are not
 * supported -- out of scope for this command. */
static size_t bnu__tr_expand_set(const char *spec, unsigned char *out, size_t max) {
    size_t count = 0;
    const char *p = spec;
    while (*p != '\0' && count < max) {
        unsigned char c;
        if (*p == '\\' && p[1] != '\0') {
            switch (p[1]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default: c = (unsigned char)p[1]; break;
            }
            p += 2;
        } else {
            c = (unsigned char)*p;
            p += 1;
        }
        if (*p == '-' && p[1] != '\0') {
            unsigned char range_end = (unsigned char)p[1];
            p += 2;
            if (range_end >= c) {
                for (unsigned int v = c; v <= range_end && count < max; ++v) out[count++] = (unsigned char)v;
            } else {
                out[count++] = c;
            }
        } else {
            out[count++] = c;
        }
    }
    return count;
}

int bnu_tr_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Translate, delete, or squeeze characters from piped stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "d");
    ap_set_opt_help(parser, "d", "Delete characters found in SET1 instead of translating them");
    ap_add_flag(parser, "s");
    ap_set_opt_help(parser, "s", "Squeeze consecutive output characters found in SET2 (or SET1 alone)");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_required_arg(parser, "set1", "Source character set (supports a-z ranges and \\n \\t \\r)");
    ap_add_optional_arg(parser, "set2", "Replacement character set");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool delete_mode = ap_found(parser, "d");
    bool squeeze_mode = ap_found(parser, "s");
    const char *set1 = ap_get_arg(parser, "set1");
    const char *set2 = ap_get_arg(parser, "set2");
    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");

    if (!delete_mode && !squeeze_mode && set2 == NULL) {
        stdio__printf("tr: missing SET2 (needed to translate; use -d or -s for a single set)\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char *end = NULL;
    unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
    bool valid_stdin_size =
        stdin_size_arg != NULL && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0';
    if (!valid_stdin_size) {
        stdio__printf("tr: missing input (tr only reads piped stdin)\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    /* set1_chars/set2_chars/map/delete_set/squeeze_set below add up to
     * roughly 1.3KB -- fine on the selftest task's stack, but this command
     * is registered with the default 4KB process stack (see main.c), where
     * that alone would eat a third of it before argument parsing's own
     * frames and stdio__read()/stdio__write() are even accounted for. Heap
     * allocate the whole working set as one block instead; only the
     * conventional 256-byte in_chunk/out_chunk I/O buffers stay on the
     * stack, matching every other bnu text command's chunked-I/O pattern. */
    struct bnu_tr_tables {
        unsigned char set1_chars[BNU_TR_SET_MAX];
        unsigned char set2_chars[BNU_TR_SET_MAX];
        unsigned char map[256];
        bool delete_set[256];
        bool squeeze_set[256];
    };
    struct bnu_tr_tables *tables = memory__calloc(1, sizeof(*tables));
    if (tables == NULL) {
        ap_free(parser);
        return BRUCE_ERR_NO_MEMORY;
    }
    size_t set1_count = bnu__tr_expand_set(set1, tables->set1_chars, BNU_TR_SET_MAX);
    size_t set2_count = set2 != NULL ? bnu__tr_expand_set(set2, tables->set2_chars, BNU_TR_SET_MAX) : 0;
    ap_free(parser);
    if (set1_count == 0 || (set2 != NULL && set2_count == 0)) {
        stdio__printf("tr: empty character set\n");
        memory__free(tables);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    for (unsigned i = 0; i < 256; ++i) tables->map[i] = (unsigned char)i;

    if (!delete_mode && set2 != NULL) {
        for (size_t i = 0; i < set1_count; ++i) {
            unsigned char replacement = tables->set2_chars[i < set2_count ? i : set2_count - 1];
            tables->map[tables->set1_chars[i]] = replacement;
        }
    }
    if (delete_mode) {
        for (size_t i = 0; i < set1_count; ++i) tables->delete_set[tables->set1_chars[i]] = true;
    }
    if (squeeze_mode) {
        const unsigned char *squeeze_source = set2 != NULL ? tables->set2_chars : tables->set1_chars;
        size_t squeeze_count = set2 != NULL ? set2_count : set1_count;
        for (size_t i = 0; i < squeeze_count; ++i) tables->squeeze_set[squeeze_source[i]] = true;
    }

    bool have_last = false;
    unsigned char last_output = 0;
    unsigned char in_chunk[256], out_chunk[256];
    size_t offset = 0;
    bruce_result_t result = BRUCE_OK;
    while (result == BRUCE_OK && offset < (size_t)parsed_stdin_size) {
        size_t remaining = (size_t)parsed_stdin_size - offset;
        size_t want = remaining > sizeof(in_chunk) ? sizeof(in_chunk) : remaining;
        size_t read_size = 0;
        result = stdio__read(in_chunk, want, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result != BRUCE_OK) break;

        size_t out_count = 0;
        for (size_t i = 0; i < read_size; ++i) {
            unsigned char c = in_chunk[i];
            if (delete_mode && tables->delete_set[c]) continue;
            unsigned char out = tables->map[c];
            if (squeeze_mode && have_last && last_output == out && tables->squeeze_set[out]) continue;
            out_chunk[out_count++] = out;
            have_last = true;
            last_output = out;
        }
        if (out_count > 0) (void)stdio__write(out_chunk, out_count);
        offset += read_size;
    }
    memory__free(tables);
    return result;
}

/* ---- tee -------------------------------------------------------------------- */

#define BNU_TEE_MAX_FILES 8

int bnu_tee_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Copy piped stdin to stdout and to one or more files.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "a");
    ap_set_opt_help(parser, "a", "Append to the files instead of overwriting them");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool append = ap_found(parser, "a");
    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");
    int file_count = ap_count_args(parser);

    char *end = NULL;
    unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
    bool valid_stdin_size =
        stdin_size_arg != NULL && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0';
    if (!valid_stdin_size) {
        stdio__printf("tee: missing input (tee only reads piped stdin)\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (file_count > BNU_TEE_MAX_FILES) {
        stdio__printf("tee: too many files (max %d)\n", BNU_TEE_MAX_FILES);
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_file_id_t files[BNU_TEE_MAX_FILES];
    int open_count = 0;
    bruce_result_t result = BRUCE_OK;
    for (int i = 0; i < file_count; ++i) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
            result = BRUCE_ERR_INVALID_PATH;
            break;
        }
        uint32_t flags = BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE |
                          (append ? BRUCE_STORAGE_OPEN_APPEND : BRUCE_STORAGE_OPEN_TRUNCATE);
        result = storage__open(path, flags, &files[open_count]);
        if (result != BRUCE_OK) {
            stdio__printf("tee: %s: error %d\n", path, result);
            break;
        }
        open_count++;
    }
    ap_free(parser);

    if (result == BRUCE_OK) {
        unsigned char chunk[256];
        size_t offset = 0;
        while (result == BRUCE_OK && offset < (size_t)parsed_stdin_size) {
            size_t remaining = (size_t)parsed_stdin_size - offset;
            size_t want = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
            size_t read_size = 0;
            result = stdio__read(chunk, want, UINT32_MAX, &read_size);
            if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
            if (result != BRUCE_OK) break;
            (void)stdio__write(chunk, read_size);
            for (int i = 0; i < open_count && result == BRUCE_OK; ++i) {
                size_t written = 0;
                result = storage__write(files[i], chunk, read_size, &written);
            }
            offset += read_size;
        }
    }
    for (int i = 0; i < open_count; ++i) (void)storage__close(files[i]);
    return result;
}

/* ---- seq -------------------------------------------------------------------- */

int bnu_seq_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Print a sequence of integers.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "s", NULL);
    ap_set_opt_help(parser, "s", "Separator between numbers (default: newline)");
    ap_add_required_arg(parser, "n1", "LAST, or FIRST if a second number follows");
    ap_add_optional_arg(parser, "n2", "LAST, or INCREMENT if a third number follows");
    ap_add_optional_arg(parser, "n3", "LAST, when FIRST and INCREMENT were both given");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *sep = ap_get_str_value(parser, "s");
    if (sep == NULL) sep = "\n";
    const char *a = ap_get_arg(parser, "n1");
    const char *b = ap_get_arg(parser, "n2");
    const char *c = ap_get_arg(parser, "n3");

    char *end = NULL;
    long first = 1, increment = 1, last = 0;
    bool valid = true;
    if (c != NULL) {
        first = strtol(a, &end, 10);
        valid = valid && end != a && *end == '\0';
        increment = strtol(b, &end, 10);
        valid = valid && end != b && *end == '\0';
        last = strtol(c, &end, 10);
        valid = valid && end != c && *end == '\0';
    } else if (b != NULL) {
        first = strtol(a, &end, 10);
        valid = valid && end != a && *end == '\0';
        last = strtol(b, &end, 10);
        valid = valid && end != b && *end == '\0';
    } else {
        last = strtol(a, &end, 10);
        valid = valid && end != a && *end == '\0';
    }
    ap_free(parser);

    if (!valid || increment == 0) {
        stdio__printf("seq: invalid arguments\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bool printed_any = false;
    if (increment > 0) {
        for (long value = first; value <= last; value += increment) {
            if (printed_any) stdio__printf("%s", sep);
            stdio__printf("%ld", value);
            printed_any = true;
        }
    } else {
        for (long value = first; value >= last; value += increment) {
            if (printed_any) stdio__printf("%s", sep);
            stdio__printf("%ld", value);
            printed_any = true;
        }
    }
    if (printed_any) stdio__printf("\n");
    return BRUCE_OK;
}
