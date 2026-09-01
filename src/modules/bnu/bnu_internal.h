#pragma once

/*
 * Helpers shared across the bnu_*_app.c translation units. Not part of
 * bnu_app.h's public entry-point surface.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "args.h"
#include "core_sdk/result.h"

/* Frees `parser` and translates its parse status into a bruce_result_t
 * (BRUCE_OK for help/version, otherwise an invalid-argument/no-memory error). */
int bnu__parse_failure(ArgParser *parser);

/* Allocates a parser with `helptext` set as its description, or NULL on
 * allocation failure. */
ArgParser *bnu__new_parser(const char *helptext);

/* Resolves `path` against the working directory (bnu__get_working_directory())
 * and collapses '.'/'..' components into `out_path`, which must be at least
 * BRUCE_STORAGE_PATH_MAX bytes. NULL or an empty `path` resolves to the
 * working directory itself. Returns false if the resolved path would not fit. */
bool bnu__resolve_path(const char *path, char *out_path);

/* Formats `bytes` into `output` as a plain decimal, or a human-readable
 * value (e.g. 8.2K, 1.3M) when `human` is true. */
void bnu__format_size(uint32_t bytes, bool human, char *output, size_t capacity);

/* Prints one xxd-compatible line. The explicit address makes this reusable
 * by a future memorydump command whose source is not a storage file. */
void bnu__xxd_print_line(
    const uint8_t *data, size_t length, uint64_t address, size_t columns, size_t group, bool plain
);

/* Loads `path`'s full contents into a memory__external_malloc buffer (an
 * empty file yields *out_data == NULL, *out_length == 0), or
 * BRUCE_ERR_RESOURCE_LIMIT if it exceeds `max_bytes`. Caller frees the
 * buffer with memory__external_free(). Shared by the bnu commands that need
 * random access to their whole input rather than a single streaming pass. */
bruce_result_t bnu__load_path(const char *path, size_t max_bytes, const void **out_data, size_t *out_length);

/* Reads exactly `size` bytes of piped stdin into a memory__external_malloc
 * buffer, per the shell's --stdin-size convention (see shell_executor.c and
 * text_app.c, which originated it). Caller frees the buffer with
 * memory__external_free(). */
bruce_result_t bnu__load_stdin(size_t size, const void **out_data, size_t *out_length);
