#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/disk.h" // IWYU pragma: export (BRUCE_DISK_NAME_MAX)
#include "core_sdk/result.h"

/*
 * Manages the "user area": the flash left over after the static partitions
 * in partitions.csv (bootloader, partition_table, nvs, coredump, factory).
 * By default that whole area is one LittleFS partition labeled "littlefs"
 * mounted at "/" - exactly today's behavior. Calling stage_create()/
 * stage_delete()/stage_format() plus commit() persists a user-defined
 * partition table instead; new/changed partitions take effect on the next
 * reboot (see reboot_required()), never live, so nothing here ever
 * unmounts or reformats storage the running system is using.
 *
 * "swap" is a reserved label: a BRUCE_PARTITION_KIND_SWAP entry is always
 * labeled "swap" and uses the exact type/subtype core/memory's flash-backed
 * swap allocator already looks for, so creating one is all that's needed to
 * enable it. "littlefs" is likewise reserved for the entry mounted at "/";
 * it can be reformatted but not deleted. Any other label is an additional
 * LittleFS volume that gets formatted but is not auto-mounted.
 *
 * Mutating calls (stage_create/stage_delete/stage_format/commit) are
 * restricted to built-in modules (see the "bparted" CLI command), the
 * same restriction core_sdk/disk.h's disk__mount()/disk__unmount() use.
 */

#define BRUCE_PARTITION_LABEL_MAX BRUCE_DISK_NAME_MAX

typedef enum {
    BRUCE_PARTITION_KIND_SWAP,
    BRUCE_PARTITION_KIND_LITTLEFS,
} bruce_partition_kind_t;

typedef struct {
    char label[BRUCE_PARTITION_LABEL_MAX];
    bruce_partition_kind_t kind;
    uint64_t offset; /* Relative to the start of the user area's data region. */
    uint64_t size;
    bool format_pending; /* Will be erased/reformatted on the next boot. */
} bruce_partition_entry_t;

/* Lists the effective partition table: what's active this boot, plus any
 * staged-but-not-yet-committed changes. Pass NULL with capacity 0 to query
 * count. */
bruce_result_t
partition_manager__list(bruce_partition_entry_t *entries, size_t capacity, size_t *out_count);

/* Bytes still available in the user area for new partitions. */
bruce_result_t partition_manager__free_space(uint64_t *out_bytes);

/* Stages a new partition. `label` must be 1-16 chars of [A-Za-z0-9_-]; a
 * BRUCE_PARTITION_KIND_SWAP entry's label must be exactly "swap". `size`
 * bytes is rounded up to the nearest 4096-byte flash sector. Returns
 * BRUCE_ERR_ALREADY_EXISTS for a duplicate label, BRUCE_ERR_RESOURCE_LIMIT
 * if the table is full or there isn't enough free space. Call commit() to
 * persist. */
bruce_result_t
partition_manager__stage_create(const char *label, bruce_partition_kind_t kind, uint64_t size_bytes);

/* Stages removal of `label`. The reserved "littlefs" root entry cannot be
 * deleted (BRUCE_ERR_PERMISSION) - reformat it instead. Freed space is not
 * reclaimed by other entries until reformatted from a clean table; new
 * partitions are always appended after the highest existing offset. */
bruce_result_t partition_manager__stage_delete(const char *label);

/* Stages an in-place erase + reformat of `label`, keeping its size/kind/
 * position. Allowed for "littlefs" (wipes and reformats root on next boot). */
bruce_result_t partition_manager__stage_format(const char *label);

/* Persists the staged table to flash. Does not itself erase or format
 * anything - that happens once, automatically, the next time the device
 * boots. */
bruce_result_t partition_manager__commit(void);

/* True once a staged change has been committed but not yet applied by a
 * reboot. */
bool partition_manager__reboot_required(void);
