#pragma once

#include <stdbool.h>

#include "core_sdk/manifest.h"
#include "core_sdk/result.h"

/*
 * JavaScript loader contract, registered with app_runner's loader registry
 * (core_sdk/loader.h) for ".js" - see migration_BruceIDF.md, "Loader
 * modules" / "JavaScript contract".  Reached generically through
 * app_runner__run_path()/app_runner__inspect_path(); these two names remain
 * public only so A7 has a concrete symbol to replace in place.
 *
 * `path` must be a normalized absolute path with no "." or ".." components
 * and must end in ".js".  `arg` is shell-style text, parsed with
 * app_runner__parse_args() the same way app_runner__run()'s `arg` is.  On
 * success js__run_path() returns a positive bruce_task_id_t; on failure it
 * returns a negative BRUCE_ERR_* value, including BRUCE_ERR_INVALID_PATH
 * (malformed/wrong-extension path) and BRUCE_ERR_NOT_FOUND (no such file).
 *
 * Stage 3 (A7) replaces this file wholesale with the real mQuickJS runner,
 * optional leading manifest parsing (via the shared manifest__parse()), and
 * task-owned VM allocation, moved into its own src/modules/loaders/js/
 * module.  Until then, js__run_path() returns BRUCE_ERR_UNSUPPORTED for a
 * path that exists and passes validation, and js__inspect_path() always
 * returns BRUCE_ERR_UNSUPPORTED, so AppRunner's built-in > ELF > JS
 * resolution order is already observable.
 */
int js__run_path(const char *path, const char *arg, bool in_background);
bruce_result_t js__inspect_path(const char *path, bruce_app_inspection_t *out_inspection);
