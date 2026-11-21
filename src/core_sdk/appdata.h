#pragma once

#include <stddef.h>

#include "core_sdk/result.h"

/**
 * @brief The app's own data folder.
 *
 * Every app has a folder for the files it keeps: /appdata/<app> on internal
 * storage, or /sdcard/appdata/<app> once moved to the SD card in App
 * Settings. <app> is the app's file name without its extension.
 *
 * A relative path passed to the storage API ("history.json", "cache/a.bin")
 * is inside this folder, and needs no `storage` permission. The folder is
 * created on first use. When it is on an SD card that is not inserted, these
 * calls return BRUCE_ERR_INVALID_STATE.
 *
 * Settings belong in app_config (core_sdk/app_config.h), not here.
 */

/**
 * @brief Copies the absolute path of the app's folder into out_path.
 *
 * @param out_path Receives the NUL-terminated path.
 * @param capacity Size of out_path in bytes.
 */
bruce_result_t appdata__get_path(char *out_path, size_t capacity);

/**
 * @brief Reads a whole file from the app's folder.
 *
 * Returns BRUCE_ERR_NOT_FOUND when the file does not exist, and
 * BRUCE_ERR_RESOURCE_LIMIT when it is larger than capacity (the first
 * capacity bytes are still read).
 *
 * @param name Path relative to the app's folder.
 * @param buffer Receives the file contents.
 * @param capacity Size of buffer in bytes.
 * @param out_size Receives the number of bytes read.
 */
bruce_result_t appdata__read(const char *name, void *buffer, size_t capacity, size_t *out_size);

/**
 * @brief Replaces a file in the app's folder.
 *
 * Writes a temporary file first, so a power loss leaves either the old or
 * the new contents.
 *
 * @param name Path relative to the app's folder.
 * @param data Bytes to write.
 * @param size Number of bytes in data.
 */
bruce_result_t appdata__write(const char *name, const void *data, size_t size);

/**
 * @brief Removes a file or an empty folder from the app's folder.
 *
 * @param name Path relative to the app's folder.
 */
bruce_result_t appdata__remove(const char *name);

/**
 * @brief Total size of the files in the app's folder.
 *
 * @param out_bytes Receives the size in bytes.
 */
bruce_result_t appdata__get_usage(size_t *out_bytes);
