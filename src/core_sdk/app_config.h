#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

/*
 * Per-app local configuration store, separate from the shared
 * /config/bruce.conf singleton (core_sdk/config.h). Each app_name gets its
 * own JSON document at /config/<app_name>.conf, addressed with a
 * dot-separated json_path (e.g. "auth.user" reaches {"auth":{"user":...}}).
 * Intermediate objects are created automatically when setting a value.
 *
 * app_name and every path segment must start with a letter or underscore and
 * contain only letters, digits, or underscores. Getters fall back to the
 * supplied default when the app has no config file, the path is absent, or
 * the stored value is a different type; setters create the file and any
 * missing intermediate objects on demand.
 */

#define BRUCE_APP_CONFIG_NAME_MAX_LEN 31
#define BRUCE_APP_CONFIG_STRING_MAX_LEN 128

bool app_config__get_bool(const char *app_name, const char *json_path, bool default_value);
bruce_result_t app_config__set_bool(const char *app_name, const char *json_path, bool value);

int app_config__get_int(const char *app_name, const char *json_path, int default_value);
bruce_result_t app_config__set_int(const char *app_name, const char *json_path, int value);

/* Copies the stored string into out_value (always NUL-terminated within
 * capacity), or default_value if the path is absent/not a string. Returns
 * whether a stored value was found. */
bool app_config__get_string(
    const char *app_name, const char *json_path, const char *default_value, char *out_value,
    size_t capacity
);
bruce_result_t app_config__set_string(const char *app_name, const char *json_path, const char *value);

size_t app_config__get_bool_array(
    const char *app_name, const char *json_path, bool *out_values, size_t capacity
);
bruce_result_t
app_config__set_bool_array(const char *app_name, const char *json_path, const bool *values, size_t count);

size_t
app_config__get_int_array(const char *app_name, const char *json_path, int *out_values, size_t capacity);
bruce_result_t
app_config__set_int_array(const char *app_name, const char *json_path, const int *values, size_t count);

/* out_values[i] must point at a writable buffer of at least value_size bytes;
 * up to `capacity` entries are filled. Returns the number of entries filled. */
size_t app_config__get_string_array(
    const char *app_name, const char *json_path, char *const *out_values, size_t value_size,
    size_t capacity
);
bruce_result_t app_config__set_string_array(
    const char *app_name, const char *json_path, const char *const *values, size_t count
);

/* Removes the value at json_path. BRUCE_ERR_NOT_FOUND if it was not set. */
bruce_result_t app_config__remove(const char *app_name, const char *json_path);
