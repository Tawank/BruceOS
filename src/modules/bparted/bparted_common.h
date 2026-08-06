#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/partition_manager.h"

/* Shared logic between bparted_cli.c and bparted_gui.c, so neither
 * duplicates the other's parsing/formatting - the GUI drives the CLI via
 * app_runner__run_command() for every mutation (see bparted_gui.c), but
 * both need to render/parse the same kind names and human-readable sizes. */

/* static inline (not defined in the .c file): so callers formatting it into
 * a fixed-size buffer (bparted_gui.c) get the literal inlined at the call
 * site, letting the compiler prove the real bound instead of treating an
 * opaque cross-TU `const char *` return as unbounded. */
static inline const char *bparted_common__kind_name(bruce_partition_kind_t kind) {
    return kind == BRUCE_PARTITION_KIND_SWAP ? "swap" : "littlefs";
}

bool bparted_common__parse_kind(const char *text, bruce_partition_kind_t *out_kind);

/* Formats `bytes` like "512K"/"2.5M". `capacity` should be at least 16. */
void bparted_common__format_size(uint64_t bytes, char *out, size_t capacity);

/* Parses sizes like "512K", "2M", or a bare byte count ("1048576"). Accepts
 * K/M/G suffixes (case-insensitive). */
bool bparted_common__parse_size(const char *text, uint64_t *out_bytes);
