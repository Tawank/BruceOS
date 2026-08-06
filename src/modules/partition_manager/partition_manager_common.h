#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/partition_manager.h"

/* Shared logic between bparted.c (the "bparted" command) and
 * partition_manager_gui.c (the "partition_manager" app), so neither
 * duplicates the other's parsing/formatting - the GUI drives the CLI via
 * app_runner__run_command() for every mutation (see partition_manager_gui.c),
 * but both need to render/parse the same kind names and human-readable
 * sizes. */

const char *partition_manager_common__kind_name(bruce_partition_kind_t kind);
bool partition_manager_common__parse_kind(const char *text, bruce_partition_kind_t *out_kind);

/* Formats `bytes` like "512K"/"2.5M". `capacity` should be at least 16. */
void partition_manager_common__format_size(uint64_t bytes, char *out, size_t capacity);

/* Parses sizes like "512K", "2M", or a bare byte count ("1048576"). Accepts
 * K/M/G suffixes (case-insensitive). */
bool partition_manager_common__parse_size(const char *text, uint64_t *out_bytes);
