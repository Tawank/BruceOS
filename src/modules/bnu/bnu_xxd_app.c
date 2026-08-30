#include "bnu_app.h"
#include "bnu_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "args.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#define BNU_XXD_DEFAULT_COLUMNS 16u
#define BNU_XXD_PLAIN_COLUMNS 30u
#define BNU_XXD_MAX_COLUMNS 32u

typedef struct {
    uint64_t offset;
    uint64_t remaining;
    bool limited;
    size_t columns;
    size_t group;
    bool plain;
} bnu_xxd_options_t;

/* Kept separate from file/stdin reading so a future privileged memorydump
 * command can render chunks obtained from RAM, PSRAM, or swap through the
 * same xxd-compatible formatter. */
void bnu__xxd_print_line(
    const uint8_t *data, size_t length, uint64_t address, size_t columns, size_t group, bool plain
) {
    static const char hex[] = "0123456789abcdef";
    char line[192];
    size_t used = 0;
    if (!plain) {
        int written = snprintf(line, sizeof(line), "%08llx:", (unsigned long long)address);
        if (written < 0) return;
        used = (size_t)written;
    }
    for (size_t i = 0; i < columns; ++i) {
        if (!plain && i % group == 0 && used + 1 < sizeof(line)) line[used++] = ' ';
        if (i < length) {
            if (used + 2 >= sizeof(line)) return;
            line[used++] = hex[data[i] >> 4];
            line[used++] = hex[data[i] & 0x0fu];
        } else if (!plain) {
            if (used + 2 >= sizeof(line)) return;
            line[used++] = ' ';
            line[used++] = ' ';
        }
    }
    if (!plain) {
        if (used + length + 3 >= sizeof(line)) return;
        line[used++] = ' ';
        line[used++] = ' ';
        for (size_t i = 0; i < length; ++i) {
            line[used++] = isprint((unsigned char)data[i]) ? (char)data[i] : '.';
        }
    }
    line[used++] = '\n';
    line[used] = '\0';
    stdio__printf("%s", line);
}

static bool bnu__xxd_parse_u64(const char *text, uint64_t *out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') return false;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (end == text || *end != '\0') return false;
    *out = (uint64_t)value;
    return true;
}

static bruce_result_t bnu__xxd_dump_file(const char *path, const bnu_xxd_options_t *options) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    if (options->offset > INT64_MAX) {
        (void)storage__close(file);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    result = storage__seek(file, (int64_t)options->offset, SEEK_SET, NULL);
    uint8_t line[BNU_XXD_MAX_COLUMNS];
    uint64_t address = options->offset;
    uint64_t remaining = options->remaining;
    while (result == BRUCE_OK && (!options->limited || remaining > 0)) {
        size_t want = options->columns;
        if (options->limited && remaining < want) want = (size_t)remaining;
        size_t read_size = 0;
        result = storage__read(file, line, want, &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        bnu__xxd_print_line(line, read_size, address, options->columns, options->group, options->plain);
        address += read_size;
        if (options->limited) remaining -= read_size;
    }
    bruce_result_t close_result = storage__close(file);
    return result != BRUCE_OK ? result : close_result;
}

static bruce_result_t bnu__xxd_dump_stdin(uint64_t stdin_size, const bnu_xxd_options_t *options) {
    uint8_t line[BNU_XXD_MAX_COLUMNS];
    uint64_t discarded = 0;
    while (discarded < options->offset && discarded < stdin_size) {
        size_t want = options->offset - discarded > sizeof(line)
                          ? sizeof(line)
                          : (size_t)(options->offset - discarded);
        if (stdin_size - discarded < want) want = (size_t)(stdin_size - discarded);
        size_t read_size = 0;
        bruce_result_t result = stdio__read(line, want, UINT32_MAX, &read_size);
        if (result != BRUCE_OK) return result;
        if (read_size == 0) return BRUCE_ERR_IO;
        discarded += read_size;
    }
    uint64_t available = stdin_size - discarded;
    uint64_t remaining = options->limited && options->remaining < available
                             ? options->remaining
                             : available;
    uint64_t address = options->offset;
    while (remaining > 0) {
        size_t want = remaining > options->columns ? options->columns : (size_t)remaining;
        size_t read_size = 0;
        bruce_result_t result = stdio__read(line, want, UINT32_MAX, &read_size);
        if (result != BRUCE_OK) return result;
        if (read_size == 0) return BRUCE_ERR_IO;
        bnu__xxd_print_line(line, read_size, address, options->columns, options->group, options->plain);
        address += read_size;
        remaining -= read_size;
    }
    return BRUCE_OK;
}

int bnu_xxd_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Make a hexadecimal dump of a file or stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "c", NULL);
    ap_set_opt_help(parser, "c", "Bytes per output line (1..32; default 16)");
    ap_add_str_opt(parser, "g", NULL);
    ap_set_opt_help(parser, "g", "Bytes per hexadecimal group (1, 2, 4, or 8; default 2)");
    ap_add_str_opt(parser, "l", NULL);
    ap_set_opt_help(parser, "l", "Stop after this many bytes");
    ap_add_str_opt(parser, "s", NULL);
    ap_set_opt_help(parser, "s", "Start at this byte offset (decimal or 0x-prefixed)");
    ap_add_flag(parser, "p");
    ap_set_opt_help(parser, "p", "Plain hexadecimal output without addresses or ASCII");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to dump (reads stdin if omitted)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bnu_xxd_options_t options = {
        .columns = ap_found(parser, "p") ? BNU_XXD_PLAIN_COLUMNS : BNU_XXD_DEFAULT_COLUMNS,
        .group = 2,
        .plain = ap_found(parser, "p"),
    };
    uint64_t parsed = 0;
    bool valid = true;
    if (ap_found(parser, "c")) {
        valid = bnu__xxd_parse_u64(ap_get_str_value(parser, "c"), &parsed) &&
                parsed >= 1 && parsed <= BNU_XXD_MAX_COLUMNS;
        if (valid) options.columns = (size_t)parsed;
    }
    if (valid && ap_found(parser, "g")) {
        valid = bnu__xxd_parse_u64(ap_get_str_value(parser, "g"), &parsed) &&
                (parsed == 1 || parsed == 2 || parsed == 4 || parsed == 8);
        if (valid) options.group = (size_t)parsed;
    }
    if (valid && ap_found(parser, "l")) {
        valid = bnu__xxd_parse_u64(ap_get_str_value(parser, "l"), &options.remaining);
        options.limited = valid;
    }
    if (valid && ap_found(parser, "s")) {
        valid = bnu__xxd_parse_u64(ap_get_str_value(parser, "s"), &options.offset);
    }
    if (!valid) {
        stdio__printf("xxd: invalid numeric option\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    const char *file_arg = ap_get_arg(parser, "file");
    bruce_result_t result;
    if (file_arg != NULL) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(file_arg, path)) result = BRUCE_ERR_INVALID_PATH;
        else result = bnu__xxd_dump_file(path, &options);
        if (result != BRUCE_OK) stdio__printf("xxd: %s: %s\n", file_arg, result__to_string(result));
    } else {
        uint64_t stdin_size = 0;
        if (!bnu__xxd_parse_u64(ap_get_str_value(parser, "stdin-size"), &stdin_size)) {
            stdio__printf("xxd: missing file operand\n");
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            result = bnu__xxd_dump_stdin(stdin_size, &options);
        }
    }
    ap_free(parser);
    return result;
}
