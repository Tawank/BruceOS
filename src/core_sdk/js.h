#pragma once

#include <stdbool.h>

/*
 * JavaScript runner contract used by AppRunner's named resolution (see
 * migration_BruceIDF.md, "Named execution") and directly by any task that
 * holds the `execute` permission to start a path outside of `/bin/<name>`.
 *
 * `path` must be a normalized absolute path with no "." or ".." components
 * and must end in ".js".  `arg` is shell-style text, parsed the same way as
 * app_runner__run()'s `arg`.  On success this returns a positive
 * bruce_task_id_t; on failure it returns a negative BRUCE_ERR_* value,
 * including BRUCE_ERR_INVALID_PATH (malformed/wrong-extension path) and
 * BRUCE_ERR_NOT_FOUND (no such file).
 *
 * Stage 3 (A7) integrates the real mQuickJS runner, optional leading
 * manifest parsing, and task-owned VM allocation.  Until then, this returns
 * BRUCE_ERR_UNSUPPORTED for a path that exists and passes validation, so
 * AppRunner's built-in > ELF > JS resolution order is already observable.
 */
int js__run_path(const char *path, const char *arg, bool in_background);
