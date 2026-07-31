#pragma once

#include <stdbool.h>

#include "shell_internal.h"

bool shell_builtins__is_builtin(const char *name);
int shell_builtins__run(shell_state_t *state, int argc, char **argv);
const char *shell_builtins__get(const shell_state_t *state, const char *name);
int shell_builtins__set(shell_state_t *state, const char *name, const char *value);
