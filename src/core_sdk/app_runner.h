#pragma once

#include <stdbool.h>

#include "core_sdk/result.h"

typedef int (*bruce_app_entry_t)(int argc, char **argv);

/* Registers a built-in command.  Returns BRUCE_ERR_ALREADY_EXISTS for a
 * duplicate name and BRUCE_ERR_RESOURCE_LIMIT if the registry is full. */
bruce_result_t app_runner__register(const char *name, bruce_app_entry_t entry);

/* Starts a named built-in, ELF, or JavaScript application.  On success this
 * returns a positive bruce_task_id_t.  On failure it returns a negative
 * BRUCE_ERR_* value (including BRUCE_ERR_NOT_FOUND and BRUCE_ERR_BUSY).
 * `arg` is shell-style text; NULL or an empty string creates argc == 0. */
int app_runner__run(const char *app_name, const char *arg, bool in_background);
