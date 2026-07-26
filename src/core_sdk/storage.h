#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/task.h"

#define BRUCE_STORAGE_PATH_MAX 192
#define BRUCE_STORAGE_NAME_MAX 96

typedef enum {
    BRUCE_STORAGE_OPEN_READ = 1u << 0,
    BRUCE_STORAGE_OPEN_WRITE = 1u << 1,
    BRUCE_STORAGE_OPEN_APPEND = 1u << 2,
    BRUCE_STORAGE_OPEN_CREATE = 1u << 3,
    BRUCE_STORAGE_OPEN_TRUNCATE = 1u << 4,
} bruce_storage_open_flags_t;

typedef enum {
    BRUCE_STORAGE_ENTRY_FILE,
    BRUCE_STORAGE_ENTRY_DIRECTORY,
} bruce_storage_entry_type_t;

typedef struct {
    char name[BRUCE_STORAGE_NAME_MAX];
    bruce_storage_entry_type_t type;
    size_t size;
} bruce_storage_entry_t;

/* Each fallible storage API returns BRUCE_OK or BRUCE_ERR_PERMISSION,
 * BRUCE_ERR_NOT_FOUND, BRUCE_ERR_INVALID_PATH, BRUCE_ERR_IO, or a related
 * BRUCE_ERR_* result.  Open files are task-owned and automatically closed at
 * task exit/kill even if the caller never calls storage__close() itself.
 *
 * storage__open()/storage__list() check the calling task's `storage`
 * permission (built-ins always pass) and additionally refuse "/bruce.json",
 * "/permissions.json", and their atomic-write ".tmp" siblings entirely -
 * BRUCE_ERR_PERMISSION - regardless of any granted permission; storage__list()
 * silently omits those two names rather than erroring. Every other mounted
 * path (LittleFS or SD) is otherwise reachable. `path` must be an absolute,
 * normalized path with no "." or ".." components, else BRUCE_ERR_INVALID_PATH.
 * A file handle can only be used by the task that opened it; any other task
 * (or a stale/unknown id) gets BRUCE_ERR_NOT_FOUND/BRUCE_ERR_PERMISSION from
 * storage__read()/write()/seek()/close(). */
bruce_result_t storage__open(const char *path, uint32_t flags, bruce_file_id_t *out_file);
bruce_result_t storage__read(bruce_file_id_t file, void *buffer, size_t capacity, size_t *out_size);
bruce_result_t storage__write(bruce_file_id_t file, const void *buffer, size_t size, size_t *out_size);
bruce_result_t storage__seek(bruce_file_id_t file, int64_t offset, int whence, uint64_t *out_position);
bruce_result_t storage__close(bruce_file_id_t file);
bruce_result_t storage__list(const char *path, bruce_storage_entry_t *entries, size_t capacity, size_t *out_count);

/* Creates one directory. The parent must already exist. Returns BRUCE_OK when
 * the path already names a directory. */
bruce_result_t storage__mkdir(const char *path);
