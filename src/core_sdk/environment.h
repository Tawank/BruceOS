#pragma once

#include <stddef.h>

#include "core_sdk/result.h"

#define BRUCE_ENVIRONMENT_MAX_VARIABLES 24
#define BRUCE_ENVIRONMENT_NAME_MAX 32
#define BRUCE_ENVIRONMENT_VALUE_MAX 128

typedef struct {
    const char *name;
    const char *value;
} bruce_environment_variable_t;

/* Global values are runtime defaults for processes created after the change.
 * They are overridden by inherited parent values and launch assignments.
 * Persistent defaults are loaded from /config/.env during boot; these setters
 * intentionally do not modify that file. */
const char *environment__global_get(const char *name);
bruce_result_t environment__global_set(const char *name, const char *value);
bruce_result_t environment__global_unset(const char *name);

/* Process environments are runtime-only and inherited as a deep copy when a
 * child is launched. Returned values are borrowed until the calling process
 * changes its environment or exits. */
const char *environment__get(const char *name);
bruce_result_t environment__set(const char *name, const char *value);
bruce_result_t environment__unset(const char *name);
size_t environment__count(void);
bruce_result_t environment__get_at(size_t index, const char **out_name, const char **out_value);
