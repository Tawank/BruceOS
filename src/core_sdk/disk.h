#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Physical block device / partition listing and mount control.
 */

#define BRUCE_DISK_NAME_MAX 17
#define BRUCE_DISK_MOUNT_POINT_MAX 16

typedef enum {
    BRUCE_DISK_TYPE_DISK,
    BRUCE_DISK_TYPE_PARTITION,
} bruce_disk_type_t;

typedef struct {
    char name[BRUCE_DISK_NAME_MAX];
    char parent[BRUCE_DISK_NAME_MAX];
    char mount_point[BRUCE_DISK_MOUNT_POINT_MAX];
    bruce_disk_type_t type;
    uint64_t offset;
    uint64_t size;
    bool removable;
    bool read_only;
} bruce_disk_entry_t;

/**
 * @brief Lists physical block devices and their partitions.
 *
 * Entries are ordered with each disk before its partitions. Pass NULL with
 * capacity 0 to query count.
 *
 * @param entries Array to receive disk/partition entries, or NULL to only query count.
 * @param capacity Number of entries the entries array can hold.
 * @param out_count Receives the total number of disks/partitions available.
 */
bruce_result_t disk__list(bruce_disk_entry_t *entries, size_t capacity, size_t *out_count);

/**
 * @brief Mounts a disk/partition.
 *
 * `name` "sd0" always mounts at "/sdcard"; any other name is looked up
 * among core/partition_manager's extra partitions (the "bparted" command)
 * and mounted at the given path, which must be an absolute path other than
 * "/" or "/sdcard".
 *
 * @param name Disk/partition name to mount, e.g. "sd0".
 * @param mount_point Absolute path to mount at (ignored for "sd0", which always mounts at "/sdcard").
 * @permission built-in only (external applications receive BRUCE_ERR_PERMISSION)
 */
bruce_result_t disk__mount(const char *name, const char *mount_point);

/**
 * @brief Unmounts a disk/partition.
 *
 * @param name_or_mount_point Disk/partition name or its current mount point.
 * @permission built-in only (external applications receive BRUCE_ERR_PERMISSION)
 */
bruce_result_t disk__unmount(const char *name_or_mount_point);
