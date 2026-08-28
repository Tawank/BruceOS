#pragma once

#include <stddef.h>

#include "core_sdk/result.h"

/**
 * @brief Global and per-process environment variables.
 */

#define BRUCE_ENVIRONMENT_MAX_VARIABLES 24
#define BRUCE_ENVIRONMENT_NAME_MAX 32
#define BRUCE_ENVIRONMENT_VALUE_MAX 128

typedef struct {
    const char *name;
    const char *value;
} bruce_environment_variable_t;

/**
 * @brief Reads a global environment default.
 *
 * Global values are runtime defaults for processes created after the
 * change. They are overridden by inherited parent values and launch
 * assignments. Persistent defaults are loaded from /config/.env during
 * boot; these setters intentionally do not modify that file.
 *
 * @param name Variable name to look up.
 */
const char *environment__global_get(const char *name);

/**
 * @brief Sets a global environment default.
 *
 * @param name Variable name to set.
 * @param value New value.
 */
bruce_result_t environment__global_set(const char *name, const char *value);

/**
 * @brief Removes a global environment default.
 *
 * @param name Variable name to remove.
 */
bruce_result_t environment__global_unset(const char *name);

/**
 * @brief Reads a variable from the calling process's own environment.
 *
 * Process environments are runtime-only and inherited as a deep copy when
 * a child is launched. The returned value is borrowed until the calling
 * process changes its environment or exits.
 *
 * @param name Variable name to look up.
 */
const char *environment__get(const char *name);

/**
 * @brief Sets a variable in the calling process's own environment.
 *
 * @param name Variable name to set.
 * @param value New value.
 */
bruce_result_t environment__set(const char *name, const char *value);

/**
 * @brief Removes a variable from the calling process's own environment.
 *
 * @param name Variable name to remove.
 */
bruce_result_t environment__unset(const char *name);

/** @brief Number of variables in the calling process's own environment. */
size_t environment__count(void);

/**
 * @brief Reads a name/value pair from the calling process's own environment by index.
 *
 * @param index Zero-based index, below environment__count().
 * @param out_name Receives the variable's name.
 * @param out_value Receives the variable's value.
 */
bruce_result_t environment__get_at(size_t index, const char **out_name, const char **out_value);
