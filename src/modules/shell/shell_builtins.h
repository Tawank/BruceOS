#pragma once

#include <stdbool.h>

#include "shell_internal.h"

bool shell_builtins__is_builtin(const char *name);
size_t shell_builtins__count(void);
const char *shell_builtins__name(size_t index);
int shell_builtins__run(shell_state_t *state, int argc, char **argv);
const char *shell_builtins__get(const shell_state_t *state, const char *name);
int shell_builtins__set(shell_state_t *state, const char *name, const char *value);
int shell_builtins__export(shell_state_t *state, const char *name);

/* Resolves `path` (NULL or "" means the current directory itself) against
 * $PWD into an absolute, "."/".."-normalized path the storage SDK will
 * accept -- the same resolution `cd`'s own argument goes through. Exported
 * so shell_executor.c's ">"/">>" redirection targets behave like any other
 * shell path argument (relative to $PWD) rather than requiring a full
 * absolute path. out_path must have room for BRUCE_STORAGE_PATH_MAX bytes;
 * returns false (leaving out_path's contents unspecified) if the resolved
 * path would not fit. */
bool shell_builtins__resolve_path(const shell_state_t *state, const char *path, char *out_path);
