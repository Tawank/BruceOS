#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "args.h"
#include "core_sdk/base64.h"
#include "core_sdk/hash.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Data encoding/checksum commands: base64, md5sum, sha256sum, crc32. The
 * digest/checksum algorithms themselves live in core_sdk/hash.h and
 * core_sdk/base64.h -- this file only handles argument parsing, file/stdin
 * I/O, and formatting output the way the real Unix tools do.
 */

/* md5sum/sha256sum/crc32 stream a piece at a time (see
 * bnu__hash_stream_stdin()/_file() below) and never hold more than this
 * much at once; base64 needs its whole input up front instead, see
 * BNU_BASE64_MAX_BYTES near its own commands further down. */
#define BNU_HASH_CHUNK_SIZE 256
#define BNU_BASE64_DEFAULT_WRAP 76

/* Shared by all four commands: parses the shell's "--stdin-size N" piping
 * convention (see bnu_less_app_main() in bnu_pager_app.c for the original
 * of this pattern). `size_arg` is the already-`ap_found()` option value;
 * returns false (after printing its own "invalid --stdin-size" line under
 * `command`) for anything that isn't a plain unsigned integer. */
static bool bnu__hash_parse_stdin_size(const char *command, const char *size_arg, unsigned long *out_size) {
    char *end = NULL;
    unsigned long parsed = strtoul(size_arg, &end, 10);
    if (size_arg[0] == '\0' || end == NULL || *end != '\0') {
        stdio__printf("%s: invalid --stdin-size\n", command);
        return false;
    }
    *out_size = parsed;
    return true;
}

typedef bruce_result_t (*bnu__hash_feed_fn)(void *context, const void *data, size_t size);

/* Reads exactly `remaining` bytes from stdin in BNU_HASH_CHUNK_SIZE pieces,
 * calling `feed` for each one -- so a piped file is digested without ever
 * holding more than one chunk in memory. */
static bruce_result_t bnu__hash_stream_stdin(unsigned long remaining, bnu__hash_feed_fn feed, void *context) {
    unsigned char buffer[BNU_HASH_CHUNK_SIZE];
    while (remaining > 0) {
        size_t want = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        size_t read_size = 0;
        bruce_result_t result = stdio__read(buffer, want, UINT32_MAX, &read_size);
        if (result != BRUCE_OK) return result;
        if (read_size == 0) return BRUCE_ERR_IO;
        result = feed(context, buffer, read_size);
        if (result != BRUCE_OK) return result;
        remaining -= read_size;
    }
    return BRUCE_OK;
}

/* Same idea as bnu__cat_file() (bnu_fs_app.c), but feeding each chunk to
 * `feed` instead of stdio__write()-ing it. */
static bruce_result_t bnu__hash_stream_file(const char *path, bnu__hash_feed_fn feed, void *context) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    unsigned char buffer[BNU_HASH_CHUNK_SIZE];
    while (result == BRUCE_OK) {
        size_t read_size = 0;
        result = storage__read(file, buffer, sizeof(buffer), &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        result = feed(context, buffer, read_size);
    }
    bruce_result_t close_result = storage__close(file);
    return result != BRUCE_OK ? result : close_result;
}

static void bnu__hash_format_hex(const uint8_t *digest, size_t size, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0Fu];
    }
    out[size * 2] = '\0';
}

/* ------------------------------------------------------------------------ */
/* md5sum / sha256sum                                                       */
/* ------------------------------------------------------------------------ */

static bruce_result_t bnu__digest_feed(void *context, const void *data, size_t size) {
    return hash__update((bruce_hash_ctx_t *)context, data, size);
}

static bruce_result_t bnu__digest_stdin(bruce_hash_algorithm_t algorithm, unsigned long size, uint8_t *out) {
    bruce_hash_ctx_t *ctx = hash__start(algorithm);
    if (ctx == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = bnu__hash_stream_stdin(size, bnu__digest_feed, ctx);
    bruce_result_t finish_result = hash__finish(ctx, out); /* always frees ctx */
    return result != BRUCE_OK ? result : finish_result;
}

static bruce_result_t bnu__digest_file(bruce_hash_algorithm_t algorithm, const char *path, uint8_t *out) {
    bruce_hash_ctx_t *ctx = hash__start(algorithm);
    if (ctx == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = bnu__hash_stream_file(path, bnu__digest_feed, ctx);
    bruce_result_t finish_result = hash__finish(ctx, out); /* always frees ctx */
    return result != BRUCE_OK ? result : finish_result;
}

/* Shared body of md5sum and sha256sum -- same shape as coreutils: one line
 * of "<hex digest>  <path>" per file argument, or "<hex digest>  -" for
 * piped stdin when no file argument was given. */
static int bnu__digest_app_main(
    int argc, char **argv, const char *command, const char *helptext, bruce_hash_algorithm_t algorithm,
    size_t digest_size
) {
    ArgParser *parser = bnu__new_parser(helptext);
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File(s) to checksum (reads stdin if omitted)");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int path_count = ap_count_args(parser);
    const char *stdin_size_arg = ap_found(parser, "stdin-size") ? ap_get_str_value(parser, "stdin-size") : NULL;
    if (path_count == 0 && stdin_size_arg == NULL) {
        stdio__printf("%s: missing file operand\n", command);
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char hex[BRUCE_HASH_SHA256_SIZE * 2 + 1];
    uint8_t digest[BRUCE_HASH_SHA256_SIZE];
    bruce_result_t final_result = BRUCE_OK;

    if (path_count == 0) {
        unsigned long stdin_size = 0;
        if (!bnu__hash_parse_stdin_size(command, stdin_size_arg, &stdin_size)) {
            final_result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            final_result = bnu__digest_stdin(algorithm, stdin_size, digest);
            if (final_result == BRUCE_OK) {
                bnu__hash_format_hex(digest, digest_size, hex);
                stdio__printf("%s  -\n", hex);
            } else {
                stdio__printf("%s: -: %s\n", command, result__to_string(final_result));
            }
        }
    } else {
        for (int i = 0; i < path_count; ++i) {
            const char *arg = ap_get_arg_at_index(parser, i);
            char path[BRUCE_STORAGE_PATH_MAX];
            if (!bnu__resolve_path(arg, path)) {
                stdio__printf("%s: %s: invalid path\n", command, arg);
                final_result = BRUCE_ERR_INVALID_PATH;
                continue;
            }
            bruce_result_t result = bnu__digest_file(algorithm, path, digest);
            if (result == BRUCE_OK) {
                bnu__hash_format_hex(digest, digest_size, hex);
                stdio__printf("%s  %s\n", hex, path);
            } else {
                stdio__printf("%s: %s: %s\n", command, path, result__to_string(result));
                final_result = result;
            }
        }
    }

    ap_free(parser);
    return final_result;
}

int bnu_md5sum_app_main(int argc, char **argv) {
    return bnu__digest_app_main(
        argc, argv, "md5sum", "Print MD5 checksums.", BRUCE_HASH_MD5, BRUCE_HASH_MD5_SIZE
    );
}

int bnu_sha256sum_app_main(int argc, char **argv) {
    return bnu__digest_app_main(
        argc, argv, "sha256sum", "Print SHA-256 checksums.", BRUCE_HASH_SHA256, BRUCE_HASH_SHA256_SIZE
    );
}

/* ------------------------------------------------------------------------ */
/* crc32                                                                    */
/* ------------------------------------------------------------------------ */

static bruce_result_t bnu__crc32_feed(void *context, const void *data, size_t size) {
    uint32_t *crc = (uint32_t *)context;
    *crc = hash__crc32(*crc, data, size);
    return BRUCE_OK;
}

int bnu_crc32_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Print CRC-32 checksums.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File(s) to checksum (reads stdin if omitted)");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int path_count = ap_count_args(parser);
    const char *stdin_size_arg = ap_found(parser, "stdin-size") ? ap_get_str_value(parser, "stdin-size") : NULL;
    if (path_count == 0 && stdin_size_arg == NULL) {
        stdio__printf("crc32: missing file operand\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_result_t final_result = BRUCE_OK;
    if (path_count == 0) {
        unsigned long stdin_size = 0;
        if (!bnu__hash_parse_stdin_size("crc32", stdin_size_arg, &stdin_size)) {
            final_result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            uint32_t crc = 0;
            final_result = bnu__hash_stream_stdin(stdin_size, bnu__crc32_feed, &crc);
            if (final_result == BRUCE_OK) stdio__printf("%08x  -\n", (unsigned)crc);
            else stdio__printf("crc32: -: %s\n", result__to_string(final_result));
        }
    } else {
        for (int i = 0; i < path_count; ++i) {
            const char *arg = ap_get_arg_at_index(parser, i);
            char path[BRUCE_STORAGE_PATH_MAX];
            if (!bnu__resolve_path(arg, path)) {
                stdio__printf("crc32: %s: invalid path\n", arg);
                final_result = BRUCE_ERR_INVALID_PATH;
                continue;
            }
            uint32_t crc = 0;
            bruce_result_t result = bnu__hash_stream_file(path, bnu__crc32_feed, &crc);
            if (result == BRUCE_OK) {
                stdio__printf("%08x  %s\n", (unsigned)crc, path);
            } else {
                stdio__printf("crc32: %s: %s\n", path, result__to_string(result));
                final_result = result;
            }
        }
    }

    ap_free(parser);
    return final_result;
}

/* ------------------------------------------------------------------------ */
/* base64                                                                   */
/* ------------------------------------------------------------------------ */

/* base64 loads its whole input into one memory__malloc() buffer up front
 * (unlike md5sum/sha256sum/crc32, which stream) since encoding/decoding
 * needs the complete input to place padding and validate trailing
 * whitespace correctly. Kept well under a typical internal heap's size so
 * this never needs memory__external_malloc()'s PSRAM/swap fallback (see
 * bnu_less_app_main()'s LESS_MAX_BYTES for the file that does need it) --
 * base64 payloads on this device (tokens, keys, small certs/blobs) are
 * expected to be modest either way. */
#define BNU_BASE64_MAX_BYTES (64u * 1024u)

static bruce_result_t bnu__base64_load_path(const char *path, uint8_t **out_data, size_t *out_length) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK && size > BNU_BASE64_MAX_BYTES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);

    uint8_t *data = NULL;
    if (result == BRUCE_OK && size > 0) {
        data = memory__malloc((size_t)size);
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    while (result == BRUCE_OK && offset < (size_t)size) {
        size_t read_size = 0;
        result = storage__read(file, data + offset, (size_t)size - offset, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        offset += read_size;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        memory__free(data);
        return result;
    }
    *out_data = data;
    *out_length = offset;
    return BRUCE_OK;
}

static bruce_result_t bnu__base64_load_stdin(size_t size, uint8_t **out_data, size_t *out_length) {
    uint8_t *data = NULL;
    bruce_result_t result = BRUCE_OK;
    if (size > 0) {
        data = memory__malloc(size);
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    while (result == BRUCE_OK && offset < size) {
        size_t read_size = 0;
        result = stdio__read(data + offset, size - offset, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        offset += read_size;
    }
    if (result != BRUCE_OK) {
        memory__free(data);
        return result;
    }
    *out_data = data;
    *out_length = offset;
    return BRUCE_OK;
}

/* Writes already-encoded base64 text to stdout, breaking it into `wrap`-
 * column lines (wrap<=0 disables wrapping) the way base64(1) does. Empty
 * input produces no output at all, matching that same reference. */
static bruce_result_t bnu__base64_write_wrapped(const char *text, size_t length, int wrap) {
    if (length == 0) return BRUCE_OK;
    if (wrap <= 0) {
        bruce_result_t result = stdio__write(text, length);
        return result == BRUCE_OK ? stdio__write("\n", 1) : result;
    }
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset > (size_t)wrap ? (size_t)wrap : length - offset;
        bruce_result_t result = stdio__write(text + offset, chunk);
        if (result == BRUCE_OK) result = stdio__write("\n", 1);
        if (result != BRUCE_OK) return result;
        offset += chunk;
    }
    return BRUCE_OK;
}

static bruce_result_t bnu__base64_run_encode(const uint8_t *data, size_t length, int wrap) {
    size_t capacity = BRUCE_BASE64_ENCODED_SIZE(length) + 1;
    char *encoded = memory__malloc(capacity);
    if (encoded == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = base64__encode(data, length, encoded, capacity);
    if (result == BRUCE_OK) result = bnu__base64_write_wrapped(encoded, capacity - 1, wrap);
    memory__free(encoded);
    return result;
}

static bruce_result_t bnu__base64_run_decode(const uint8_t *data, size_t length) {
    size_t capacity = BRUCE_BASE64_DECODED_SIZE(length);
    uint8_t *decoded = capacity > 0 ? memory__malloc(capacity) : NULL;
    if (capacity > 0 && decoded == NULL) return BRUCE_ERR_NO_MEMORY;

    size_t decoded_size = 0;
    bruce_result_t result = base64__decode((const char *)data, length, decoded, capacity, &decoded_size);
    if (result == BRUCE_OK && decoded_size > 0) result = stdio__write(decoded, decoded_size);
    memory__free(decoded);
    return result;
}

int bnu_base64_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Base64 encode or decode a file, or piped stdin.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "d");
    ap_set_opt_help(parser, "d", "Decode instead of encode");
    ap_add_int_opt(parser, "w", BNU_BASE64_DEFAULT_WRAP);
    ap_set_opt_help(parser, "w", "Wrap encoded output at this many columns, 0 for no wrapping");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to encode/decode (reads stdin if omitted)");
    ap_unknown_options_as_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool decode = ap_found(parser, "d");
    int wrap = ap_get_int_value(parser, "w");
    const char *path_arg = ap_get_arg(parser, "file");
    const char *stdin_size_arg = ap_found(parser, "stdin-size") ? ap_get_str_value(parser, "stdin-size") : NULL;
    bool from_stdin = path_arg == NULL;
    char path[BRUCE_STORAGE_PATH_MAX] = {0};
    bool path_resolved = from_stdin || bnu__resolve_path(path_arg, path);

    if (from_stdin && stdin_size_arg == NULL) {
        stdio__printf("base64: missing file operand\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!from_stdin && !path_resolved) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_PATH;
    }
    unsigned long stdin_size = 0;
    if (from_stdin && !bnu__hash_parse_stdin_size("base64", stdin_size_arg, &stdin_size)) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (from_stdin && stdin_size > BNU_BASE64_MAX_BYTES) {
        stdio__printf("base64: input too large (max %u bytes)\n", (unsigned)BNU_BASE64_MAX_BYTES);
        ap_free(parser);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    ap_free(parser);

    uint8_t *data = NULL;
    size_t length = 0;
    bruce_result_t result = from_stdin ? bnu__base64_load_stdin((size_t)stdin_size, &data, &length)
                                        : bnu__base64_load_path(path, &data, &length);
    if (result != BRUCE_OK) {
        stdio__printf("base64: %s: %s\n", from_stdin ? "-" : path, result__to_string(result));
        return result;
    }

    result = decode ? bnu__base64_run_decode(data, length) : bnu__base64_run_encode(data, length, wrap);
    memory__free(data);
    if (result != BRUCE_OK) stdio__printf("base64: %s\n", result__to_string(result));
    return result;
}
