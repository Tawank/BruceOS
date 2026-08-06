#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/partition_manager.h"
#include "core_sdk/result.h"

/* Wording and number formatting shared by bparted_cli.c and bparted_gui.c,
 * so the two front ends describe the same layout the same way. Neither one
 * holds any partition logic of its own: both call
 * core_sdk/partition_manager.h directly, which is where every rule about
 * what a layout may look like lives. */

/* static inline (not defined in the .c file): so callers formatting the
 * result into a fixed-size buffer get the literal inlined at the call site,
 * letting the compiler prove the real bound instead of treating an opaque
 * cross-TU `const char *` return as unbounded. Same for state_name() below. */
static inline const char *bparted_common__kind_name(bruce_partition_kind_t kind) {
    return kind == BRUCE_PARTITION_KIND_SWAP ? "swap" : "littlefs";
}

/* How one entry of the next-boot layout differs from the running one, as a
 * word both front ends show verbatim ("new", "delete", "format"). An entry
 * that is not changing returns "" - callers test for that to decide whether
 * to render a change marker at all. Never NULL. */
static inline const char *bparted_common__state_name(bruce_partition_state_t state) {
    switch (state) {
    case BRUCE_PARTITION_STATE_NEW: return "new";
    case BRUCE_PARTITION_STATE_DELETED: return "delete";
    case BRUCE_PARTITION_STATE_FORMAT: return "format";
    case BRUCE_PARTITION_STATE_UNCHANGED: break;
    }
    return "";
}

bool bparted_common__parse_kind(const char *text, bruce_partition_kind_t *out_kind);

/* Formats `bytes` like "512K"/"2.5M". `capacity` should be at least 16. */
void bparted_common__format_size(uint64_t bytes, char *out, size_t capacity);

/* Parses sizes like "512K", "2M", or a bare byte count ("1048576"). Accepts
 * K/M/G suffixes (case-insensitive). */
bool bparted_common__parse_size(const char *text, uint64_t *out_bytes);

/* Why a partition_manager__* call failed, phrased for whoever is holding
 * the device rather than as a bare error number. Never NULL. */
const char *bparted_common__error_text(bruce_result_t result);

/* Both front ends read a whole layout into one stack array. The planned
 * layout needs the larger bound: partition_manager__list_planned() returns
 * the entries the next boot will have *plus* a row per entry that is about
 * to disappear, so it can be longer than any single layout. */
#define BPARTED_LAYOUT_MAX BRUCE_PARTITION_MAX_ENTRIES
#define BPARTED_PLANNED_MAX (BRUCE_PARTITION_MAX_ENTRIES * 2)

/* Normalizes a list_current()/list_planned() result for a caller that would
 * rather render a truncated layout than fail outright: clamps `*count` to
 * what actually landed in the buffer and folds BRUCE_ERR_RESOURCE_LIMIT
 * (which only says "there was more") into BRUCE_OK. */
bruce_result_t bparted_common__clamp_list(bruce_result_t result, size_t *count, size_t capacity);
