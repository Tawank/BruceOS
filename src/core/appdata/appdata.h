#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/appdata.h" // IWYU pragma: export

/* Places a relative storage path in the caller's appdata folder (created if
 * needed) and returns it in *out_path, allocated; the caller frees it. */
bruce_result_t appdata__resolve(const char *path, char **out_path);

/* App Settings, by the app's permission key ("maps.elf"). */
bruce_result_t appdata__app_path(const char *app, char *out_path, size_t capacity);
bool appdata__app_on_sd(const char *app);
bruce_result_t appdata__app_usage(const char *app, size_t *out_bytes);
/* BRUCE_ERR_BUSY while the app runs, BRUCE_ERR_INVALID_STATE without an SD card. */
bruce_result_t appdata__move(const char *app, bool to_sd);
bruce_result_t appdata__clear(const char *app);

/* Where new apps get their folder. */
bool appdata__default_on_sd(void);
bruce_result_t appdata__set_default_on_sd(bool on_sd);
