#pragma once

#include "shell_internal.h"

/* Evaluates the shell's `test`/`[`/`[[` builtins: a small, POSIX-ish subset
 * of bash's conditional expressions (see shell_condition.c for exactly what
 * is supported). Not part of the public core_sdk/ API. */

/* `argv[0]` is "test", "[", or "[["; `argv[1..argc)` is the expression,
 * including a trailing "]"/"]]" for the bracket forms. `state` resolves a
 * file-test operand's path against $PWD (see shell_builtins__resolve_path()),
 * same as any other shell path argument. Returns 0 (true), 1 (false), or 2
 * (usage/syntax error), matching test(1)'s own exit codes. */
int shell_condition__run(shell_state_t *state, int argc, char **argv);
