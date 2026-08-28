#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

/**
 * @brief Per-app local configuration store.
 *
 * Separate from the shared /config/bruce.conf singleton (core_sdk/config.h).
 * Each app_name gets its own JSON document at /config/<app_name>.conf,
 * addressed with a dot-separated json_path (e.g. "auth.user" reaches
 * {"auth":{"user":...}}). Intermediate objects are created automatically
 * when setting a value.
 *
 * app_name and every path segment must start with a letter or underscore and
 * contain only letters, digits, or underscores. Getters fall back to the
 * supplied default when the app has no config file, the path is absent, or
 * the stored value is a different type; setters create the file and any
 * missing intermediate objects on demand.
 */

#define BRUCE_APP_CONFIG_NAME_MAX_LEN 31
#define BRUCE_APP_CONFIG_STRING_MAX_LEN 128

/**
 * @brief Reads a boolean value, falling back to default_value.
 *
 * @param app_name Name of the app whose config file to read.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param default_value Value to return if the path is absent or not a bool.
 */
bool app_config__get_bool(const char *app_name, const char *json_path, bool default_value);

/**
 * @brief Sets a boolean value, creating the file/path if needed.
 *
 * @param app_name Name of the app whose config file to write.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param value New boolean value to store.
 */
bruce_result_t app_config__set_bool(const char *app_name, const char *json_path, bool value);

/**
 * @brief Reads an integer value, falling back to default_value.
 *
 * @param app_name Name of the app whose config file to read.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param default_value Value to return if the path is absent or not an int.
 */
int app_config__get_int(const char *app_name, const char *json_path, int default_value);

/**
 * @brief Sets an integer value, creating the file/path if needed.
 *
 * @param app_name Name of the app whose config file to write.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param value New integer value to store.
 */
bruce_result_t app_config__set_int(const char *app_name, const char *json_path, int value);

/**
 * @brief Copies the stored string into out_value.
 *
 * Always NUL-terminated within capacity, or default_value if the path is
 * absent/not a string. Returns whether a stored value was found.
 *
 * @param app_name Name of the app whose config file to read.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param default_value String copied into out_value when the path is absent or not a string.
 * @param out_value Buffer to receive the NUL-terminated string.
 * @param capacity Size of out_value in bytes.
 */
bool app_config__get_string(
    const char *app_name, const char *json_path, const char *default_value, char *out_value, size_t capacity
);

/**
 * @brief Sets a string value, creating the file/path if needed.
 *
 * @param app_name Name of the app whose config file to write.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param value New string value to store.
 */
bruce_result_t app_config__set_string(const char *app_name, const char *json_path, const char *value);

/**
 * @brief Serializes the stored JSON value at json_path into out_json.
 *
 * Falls back to default_json when the path is absent. This is intended for
 * structured per-app values such as arrays of objects while keeping the
 * public app_config API small. Returns whether a stored value was found.
 *
 * @param app_name Name of the app whose config file to read.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param default_json JSON text copied into out_json when the path is absent.
 * @param out_json Buffer to receive the serialized JSON text.
 * @param capacity Size of out_json in bytes.
 */
bool app_config__get_json(
    const char *app_name, const char *json_path, const char *default_json, char *out_json, size_t capacity
);

/**
 * @brief Sets a raw JSON value, creating the file/path if needed.
 *
 * @param app_name Name of the app whose config file to write.
 * @param json_path Dot-separated path to the value within the app's config document.
 * @param value_json JSON text to store at json_path.
 */
bruce_result_t app_config__set_json(const char *app_name, const char *json_path, const char *value_json);

/**
 * @brief Fills out_values with up to `capacity` bool entries from an array value.
 *
 * Returns the number of entries filled.
 *
 * @param app_name Name of the app whose config file to read.
 * @param json_path Dot-separated path to the array value.
 * @param out_values Array to receive the stored bool entries.
 * @param capacity Number of entries out_values can hold.
 */
size_t
app_config__get_bool_array(const char *app_name, const char *json_path, bool *out_values, size_t capacity);

/**
 * @brief Sets a bool array value, creating the file/path if needed.
 *
 * @param app_name Name of the app whose config file to write.
 * @param json_path Dot-separated path to the array value.
 * @param values Bool entries to store.
 * @param count Number of entries in values.
 */
bruce_result_t
app_config__set_bool_array(const char *app_name, const char *json_path, const bool *values, size_t count);

/**
 * @brief Fills out_values with up to `capacity` int entries from an array value.
 *
 * Returns the number of entries filled.
 *
 * @param app_name Name of the app whose config file to read.
 * @param json_path Dot-separated path to the array value.
 * @param out_values Array to receive the stored int entries.
 * @param capacity Number of entries out_values can hold.
 */
size_t
app_config__get_int_array(const char *app_name, const char *json_path, int *out_values, size_t capacity);

/**
 * @brief Sets an int array value, creating the file/path if needed.
 *
 * @param app_name Name of the app whose config file to write.
 * @param json_path Dot-separated path to the array value.
 * @param values Int entries to store.
 * @param count Number of entries in values.
 */
bruce_result_t
app_config__set_int_array(const char *app_name, const char *json_path, const int *values, size_t count);

/**
 * @brief Fills out_values with up to `capacity` string entries.
 *
 * out_values[i] must point at a writable buffer of at least value_size
 * bytes. Returns the number of entries filled.
 *
 * @param app_name Name of the app whose config file to read.
 * @param json_path Dot-separated path to the array value.
 * @param out_values Array of caller-owned buffers to receive the strings.
 * @param value_size Size in bytes of each buffer pointed to by out_values.
 * @param capacity Number of entries out_values can hold.
 */
size_t app_config__get_string_array(
    const char *app_name, const char *json_path, char *const *out_values, size_t value_size, size_t capacity
);

/**
 * @brief Sets a string array value, creating the file/path if needed.
 *
 * @param app_name Name of the app whose config file to write.
 * @param json_path Dot-separated path to the array value.
 * @param values String entries to store.
 * @param count Number of entries in values.
 */
bruce_result_t app_config__set_string_array(
    const char *app_name, const char *json_path, const char *const *values, size_t count
);

/**
 * @brief Removes the value at json_path.
 *
 * BRUCE_ERR_NOT_FOUND if it was not set.
 *
 * @param app_name Name of the app whose config file to modify.
 * @param json_path Dot-separated path to the value to remove.
 */
bruce_result_t app_config__remove(const char *app_name, const char *json_path);
