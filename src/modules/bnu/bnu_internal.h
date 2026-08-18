#pragma once

/*
 * Helpers shared across the bnu_*_app.c translation units. Not part of
 * bnu_app.h's public entry-point surface.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "args.h"

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
