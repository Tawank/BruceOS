#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

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

/* Lists physical block devices and their partitions. Entries are ordered with
 * each disk before its partitions. Pass NULL with capacity 0 to query count. */
bruce_result_t disk__list(bruce_disk_entry_t *entries, size_t capacity, size_t *out_count);

/* Mount and unmount are global built-in operations. Currently only sd0 at
 * /sdcard is supported; external applications receive BRUCE_ERR_PERMISSION. */
bruce_result_t disk__mount(const char *name, const char *mount_point);
bruce_result_t disk__unmount(const char *name_or_mount_point);
