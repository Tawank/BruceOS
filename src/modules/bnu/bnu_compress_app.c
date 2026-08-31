#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/compress.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * gzip/gunzip: single-file ".gz" compress/decompress, wrapping
 * core_sdk/compress.h's GZIP format directly (not core_sdk/archive.h -
 * these never touch tar framing, see the `tar`/`zip`/`unzip`/`archive`
 * commands in bnu_archive_app.c for that). Same streaming drain-loop shape
 * as core/archive/archive.c's own gzip read/write paths, just against two
 * plain storage files instead of a tar backend.
 */

#define BNU_GZIP_CHUNK_SIZE 512

/* Feeds one chunk of `in_size` bytes of `in` through `ctx` (a compress or
 * decompress context - both share the same output-side shape) and out to
 * `out_file`, looping until every pending output byte for this chunk has
 * been drained. `finish` is compress__update()'s own flag; ignored (there's
 * no equivalent) when `compressing` is false. */
static bruce_result_t bnu__gzip_pump(
    bruce_compress_ctx_t *ctx, bool compressing, const void *in, size_t in_size, bool finish,
    bruce_file_id_t out_file, bool *out_finished
) {
    uint8_t out_buf[BNU_GZIP_CHUNK_SIZE];
    bool fed = false;
    *out_finished = false;
    for (;;) {
        size_t written = 0;
        bruce_result_t result =
            compressing
                ? compress__update(
                      ctx, fed ? NULL : in, fed ? 0 : in_size, finish, out_buf, sizeof(out_buf), &written,
                      out_finished
                  )
                : decompress__update(
                      ctx, fed ? NULL : in, fed ? 0 : in_size, out_buf, sizeof(out_buf), &written, out_finished
                  );
        fed = true;
        if (result != BRUCE_OK) return result;
        if (written > 0) {
            size_t stored = 0;
            result = storage__write(out_file, out_buf, written, &stored);
            if (result == BRUCE_OK && stored != written) result = BRUCE_ERR_IO;
            if (result != BRUCE_OK) return result;
        }
        if (*out_finished) return BRUCE_OK;
        if (!finish && written < sizeof(out_buf)) return BRUCE_OK; /* nothing more pending this round */
    }
}

static bruce_result_t bnu__gzip_compress_file(const char *in_path, const char *out_path, int level) {
    bruce_file_id_t in_file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(in_path, BRUCE_STORAGE_OPEN_READ, &in_file);
    if (result != BRUCE_OK) return result;

    bruce_file_id_t out_file = BRUCE_FILE_ID_INVALID;
    result = storage__open(
        out_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &out_file
    );
    if (result != BRUCE_OK) {
        (void)storage__close(in_file);
        return result;
    }

    bruce_compress_ctx_t *ctx = compress__start(BRUCE_COMPRESS_FORMAT_GZIP, level);
    if (ctx == NULL) {
        (void)storage__close(in_file);
        (void)storage__close(out_file);
        (void)storage__remove(out_path);
        return BRUCE_ERR_NO_MEMORY;
    }

    uint8_t in_buf[BNU_GZIP_CHUNK_SIZE];
    bool finished = false;
    while (result == BRUCE_OK && !finished) {
        size_t read_size = 0;
        result = storage__read(in_file, in_buf, sizeof(in_buf), &read_size);
        if (result != BRUCE_OK) break;
        bool finish = read_size == 0;
        result = bnu__gzip_pump(ctx, true, in_buf, read_size, finish, out_file, &finished);
        if (finish) break; /* bnu__gzip_pump() already drained to *out_finished when finish=true */
    }

    compress__end(ctx);
    (void)storage__close(in_file);
    (void)storage__close(out_file);
    if (result != BRUCE_OK) (void)storage__remove(out_path);
    return result;
}

static bruce_result_t bnu__gzip_decompress_file(const char *in_path, const char *out_path) {
    bruce_file_id_t in_file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(in_path, BRUCE_STORAGE_OPEN_READ, &in_file);
    if (result != BRUCE_OK) return result;

    bruce_file_id_t out_file = BRUCE_FILE_ID_INVALID;
    result = storage__open(
        out_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &out_file
    );
    if (result != BRUCE_OK) {
        (void)storage__close(in_file);
        return result;
    }

    bruce_compress_ctx_t *ctx = decompress__start(BRUCE_COMPRESS_FORMAT_GZIP);
    if (ctx == NULL) {
        (void)storage__close(in_file);
        (void)storage__close(out_file);
        (void)storage__remove(out_path);
        return BRUCE_ERR_NO_MEMORY;
    }

    uint8_t in_buf[BNU_GZIP_CHUNK_SIZE];
    bool finished = false;
    while (result == BRUCE_OK && !finished) {
        size_t read_size = 0;
        result = storage__read(in_file, in_buf, sizeof(in_buf), &read_size);
        if (result != BRUCE_OK) break;
        if (read_size == 0) {
            result = BRUCE_ERR_IO; /* input ended before the gzip stream reported finished - truncated */
            break;
        }
        result = bnu__gzip_pump(ctx, false, in_buf, read_size, false, out_file, &finished);
    }

    decompress__end(ctx);
    (void)storage__close(in_file);
    (void)storage__close(out_file);
    if (result != BRUCE_OK) (void)storage__remove(out_path);
    return result;
}

/* True if `path` ends with ".gz" (case-sensitive, matching every other
 * extension check in this codebase - see filetype__extension_of()). */
static bool bnu__gzip_has_suffix(const char *path) {
    size_t length = strlen(path);
    return length > 3 && strcmp(path + length - 3, ".gz") == 0;
}

static int bnu__gzip_app_main(int argc, char **argv, bool decompress_default) {
    const char *command = decompress_default ? "gunzip" : "gzip";
    ArgParser *parser = bnu__new_parser(
        decompress_default ? "Decompress a \".gz\" file." : "Compress a file to \".gz\"."
    );
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (!decompress_default) {
        ap_add_flag(parser, "d");
        ap_set_opt_help(parser, "d", "Decompress instead of compress");
        ap_add_int_opt(parser, "level", BRUCE_COMPRESS_LEVEL_DEFAULT);
        ap_set_opt_help(parser, "level", "Compression level, 0 (store) to 9 (smallest/slowest)");
    }
    ap_add_flag(parser, "k");
    ap_set_opt_help(parser, "k", "Keep the input file instead of removing it");
    ap_add_required_arg(parser, "file", decompress_default ? "\".gz\" file to decompress" : "File to compress");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool decompress = decompress_default || ap_found(parser, "d");
    bool keep = ap_found(parser, "k");
    int level = decompress_default ? BRUCE_COMPRESS_LEVEL_DEFAULT : ap_get_int_value(parser, "level");
    char in_path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "file"), in_path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    char out_path[BRUCE_STORAGE_PATH_MAX];
    bruce_result_t result;
    if (decompress) {
        if (!bnu__gzip_has_suffix(in_path)) {
            stdio__printf("%s: %s: unknown suffix, ignored\n", command, in_path);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        size_t stem_length = strlen(in_path) - 3;
        if (stem_length >= sizeof(out_path)) return BRUCE_ERR_RESOURCE_LIMIT;
        memcpy(out_path, in_path, stem_length);
        out_path[stem_length] = '\0';
        result = bnu__gzip_decompress_file(in_path, out_path);
    } else {
        int written = snprintf(out_path, sizeof(out_path), "%s.gz", in_path);
        if (written < 0 || (size_t)written >= sizeof(out_path)) return BRUCE_ERR_RESOURCE_LIMIT;
        result = bnu__gzip_compress_file(in_path, out_path, level);
    }

    if (result != BRUCE_OK) {
        stdio__printf("%s: %s: %s\n", command, in_path, result__to_string(result));
        return result;
    }
    if (!keep) result = storage__remove(in_path);
    return result;
}

int bnu_gzip_app_main(int argc, char **argv) { return bnu__gzip_app_main(argc, argv, false); }

int bnu_gunzip_app_main(int argc, char **argv) { return bnu__gzip_app_main(argc, argv, true); }
