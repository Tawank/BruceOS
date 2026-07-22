#pragma once

#include <stdbool.h>

/*
 * ELF loader contract used by AppRunner's named resolution (see
 * migration_BruceIDF.md, "Named execution") and directly by any task that
 * holds the `execute` permission to start a path outside of `/bin/<name>`.
 *
 * `path` must be a normalized absolute path with no "." or ".." components
 * and must end in ".elf".  `arg` is shell-style text, parsed the same way as
 * app_runner__run()'s `arg`.  On success this returns a positive
 * bruce_task_id_t; on failure it returns a negative BRUCE_ERR_* value,
 * including BRUCE_ERR_INVALID_PATH (malformed/wrong-extension path) and
 * BRUCE_ERR_NOT_FOUND (no such file).
 *
 * Stage 3 (A6) integrates the real Espressif ELF loader, `.bruce.manifest`
 * validation, and task-owned loader allocations.  Until then, this returns
 * BRUCE_ERR_UNSUPPORTED for a path that exists and passes validation, so
 * AppRunner's built-in > ELF > JS resolution order is already observable.
 */
int elf__run_path(const char *path, const char *arg, bool in_background);
