#include "bnu_app.h"
#include "bnu_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

#include "args.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Text statistics command: wc.
 *
 * Counts lines, words, and bytes. Unlike `less`/`grep`, wc never needs
 * random access to what it reads, so it streams each source in fixed-size
 * chunks instead of loading it fully into memory first -- there's no
 * matching size cap here for that reason.
 */

typedef struct {
    uint64_t lines;
    uint64_t words;
    uint64_t bytes;
    bool in_word;
} bnu_wc_counts_t;

static void bnu__wc_count_chunk(bnu_wc_counts_t *counts, const unsigned char *data, size_t length) {
    counts->bytes += length;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = data[i];
        if (c == '\n') counts->lines++;
        if (isspace(c)) {
            counts->in_word = false;
        } else if (!counts->in_word) {
            counts->in_word = true;
            counts->words++;
        }
    }
}

static bruce_result_t bnu__wc_count_path(const char *path, bnu_wc_counts_t *counts) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    unsigned char chunk[256];
    while (result == BRUCE_OK) {
        size_t read_size = 0;
        result = storage__read(file, chunk, sizeof(chunk), &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        bnu__wc_count_chunk(counts, chunk, read_size);
    }
    bruce_result_t close_result = storage__close(file);
    return result != BRUCE_OK ? result : close_result;
}

static bruce_result_t bnu__wc_count_stdin(size_t size, bnu_wc_counts_t *counts) {
    unsigned char chunk[256];
    size_t offset = 0;
    bruce_result_t result = BRUCE_OK;
    while (result == BRUCE_OK && offset < size) {
        size_t want = size - offset > sizeof(chunk) ? sizeof(chunk) : size - offset;
        size_t read_size = 0;
        result = stdio__read(chunk, want, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) bnu__wc_count_chunk(counts, chunk, read_size);
        offset += read_size;
    }
    return result;
}

/* Fields print in the fixed lines/words/bytes order regardless of the
 * order -l/-w/-c were given on the command line, matching real wc. */
static void bnu__wc_print(
    bool show_lines, bool show_words, bool show_bytes, const bnu_wc_counts_t *counts, const char *filename
) {
    if (show_lines) stdio__printf("%7llu", (unsigned long long)counts->lines);
    if (show_words) stdio__printf("%7llu", (unsigned long long)counts->words);
    if (show_bytes) stdio__printf("%7llu", (unsigned long long)counts->bytes);
    if (filename != NULL) stdio__printf(" %s", filename);
    stdio__printf("\n");
}

int bnu_wc_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Count lines, words, and bytes in files or stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "l");
    ap_set_opt_help(parser, "l", "Print only the newline count");
    ap_add_flag(parser, "w");
    ap_set_opt_help(parser, "w", "Print only the word count");
    ap_add_flag(parser, "c");
    ap_set_opt_help(parser, "c", "Print only the byte count");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool show_lines = ap_found(parser, "l");
    bool show_words = ap_found(parser, "w");
    bool show_bytes = ap_found(parser, "c");
    if (!show_lines && !show_words && !show_bytes) show_lines = show_words = show_bytes = true;

    const char *stdin_size_arg = ap_get_str_value(parser, "stdin-size");
    int file_count = ap_count_args(parser);

    bruce_result_t result = BRUCE_OK;
    bnu_wc_counts_t total = {0};

    if (file_count == 0) {
        char *end = NULL;
        unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
        bool valid_stdin_size = stdin_size_arg != NULL && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0';
        if (!valid_stdin_size) {
            stdio__printf("wc: missing filename\n");
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            bnu_wc_counts_t counts = {0};
            result = bnu__wc_count_stdin((size_t)parsed_stdin_size, &counts);
            if (result != BRUCE_OK) {
                stdio__printf("wc: (standard input): %s\n", result__to_string(result));
            } else {
                bnu__wc_print(show_lines, show_words, show_bytes, &counts, NULL);
            }
        }
    } else {
        for (int i = 0; i < file_count && result == BRUCE_OK; ++i) {
            char path[BRUCE_STORAGE_PATH_MAX];
            if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
                result = BRUCE_ERR_INVALID_PATH;
                break;
            }
            bnu_wc_counts_t counts = {0};
            result = bnu__wc_count_path(path, &counts);
            if (result != BRUCE_OK) {
                stdio__printf("wc: %s: %s\n", path, result__to_string(result));
                break;
            }
            bnu__wc_print(show_lines, show_words, show_bytes, &counts, path);
            total.lines += counts.lines;
            total.words += counts.words;
            total.bytes += counts.bytes;
        }
        if (result == BRUCE_OK && file_count > 1) {
            bnu__wc_print(show_lines, show_words, show_bytes, &total, "total");
        }
    }

    ap_free(parser);
    return result;
}
