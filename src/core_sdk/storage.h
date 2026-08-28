#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/process.h"
#include "core_sdk/result.h"

/**
 * @brief File and directory access.
 *
 * Each fallible storage API returns BRUCE_OK or BRUCE_ERR_PERMISSION,
 * BRUCE_ERR_NOT_FOUND, BRUCE_ERR_INVALID_PATH, BRUCE_ERR_IO, or a related
 * BRUCE_ERR_* result. Open files are process-owned and automatically
 * closed at process exit/kill even if the caller never calls
 * storage__close() itself.
 *
 * storage__open()/storage__list() check the calling process's `storage`
 * permission (built-ins always pass) and additionally refuse
 * "/config/bruce.conf", "/config/permissions.json", and their atomic-write
 * ".tmp" siblings entirely - BRUCE_ERR_PERMISSION - regardless of any
 * granted permission; storage__list() silently omits those two names
 * rather than erroring. Every other mounted path (LittleFS or SD) is
 * otherwise reachable. `path` must be an absolute, normalized path with no
 * "." or ".." components, else BRUCE_ERR_INVALID_PATH. A file handle can
 * only be used by the process that opened it; any other process (or a
 * stale/unknown id) gets BRUCE_ERR_NOT_FOUND/BRUCE_ERR_PERMISSION from
 * storage__read()/write()/seek()/close().
 */

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

/**
 * @brief Reports whether a path exists.
 *
 * Applies the same permission, normalization, and protected-path policy as
 * storage__open().
 *
 * @param path Path to check.
 * @param out_exists Receives whether the path exists.
 * @permission storage
 */
bruce_result_t storage__exists(const char *path, bool *out_exists);

/**
 * @brief Opens a file.
 *
 * See the module-level notes above for the permission, normalization, and
 * protected-path policy both this and storage__list() share.
 *
 * @param path Absolute, normalized path to open.
 * @param flags Bitwise OR of bruce_storage_open_flags_t values.
 * @param out_file Receives the new file handle.
 * @permission storage
 */
bruce_result_t storage__open(const char *path, uint32_t flags, bruce_file_id_t *out_file);

/**
 * @brief Reads from an open file.
 *
 * @param file File handle from storage__open().
 * @param buffer Buffer to receive read bytes.
 * @param capacity Size of buffer in bytes.
 * @param out_size Receives the number of bytes read.
 */
bruce_result_t storage__read(bruce_file_id_t file, void *buffer, size_t capacity, size_t *out_size);

/**
 * @brief Writes to an open file.
 *
 * @param file File handle from storage__open().
 * @param buffer Bytes to write.
 * @param size Number of bytes in buffer.
 * @param out_size Receives the number of bytes written.
 */
bruce_result_t storage__write(bruce_file_id_t file, const void *buffer, size_t size, size_t *out_size);

/**
 * @brief Seeks an open file.
 *
 * @param file File handle from storage__open().
 * @param offset Offset to seek by, interpreted per whence.
 * @param whence Reference point for offset (SEEK_SET/SEEK_CUR/SEEK_END).
 * @param out_position Receives the new absolute file position.
 */
bruce_result_t storage__seek(bruce_file_id_t file, int64_t offset, int whence, uint64_t *out_position);

/**
 * @brief Closes an open file.
 *
 * @param file File handle to close.
 */
bruce_result_t storage__close(bruce_file_id_t file);

/**
 * @brief Lists a directory.
 *
 * See the module-level notes above for the permission, normalization, and
 * protected-path policy both this and storage__open() share.
 *
 * @param path Absolute, normalized directory path to list.
 * @param entries Array to receive directory entries.
 * @param capacity Number of entries the entries array can hold.
 * @param out_count Receives the total number of entries available.
 * @permission storage
 */
bruce_result_t
storage__list(const char *path, bruce_storage_entry_t *entries, size_t capacity, size_t *out_count);

/**
 * @brief Creates one directory.
 *
 * The parent must already exist. Returns BRUCE_OK when the path already
 * names a directory.
 *
 * @param path Directory path to create.
 */
bruce_result_t storage__mkdir(const char *path);

/**
 * @brief Removes one file or empty directory.
 *
 * Root and protected configuration files are never removable through the
 * public SDK.
 *
 * @param path Path of the file or empty directory to remove.
 */
bruce_result_t storage__remove(const char *path);

/**
 * @brief Renames a file or directory without replacing an existing destination.
 *
 * Both paths must be on the same mounted filesystem.
 *
 * @param from Existing path.
 * @param to New path; must not already exist.
 */
bruce_result_t storage__rename(const char *from, const char *to);

/**
 * @brief Returns filesystem capacity and used bytes for the volume containing path.
 *
 * @param path Path identifying which mounted volume to report on.
 * @param total_bytes Receives the volume's total capacity in bytes.
 * @param used_bytes Receives the volume's used bytes.
 */
bruce_result_t storage__get_usage(const char *path, size_t *total_bytes, size_t *used_bytes);
