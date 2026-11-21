#pragma once

/* Core-private parts of the permission store. Every public permission__*
 * API is declared in "core_sdk/permission.h"; this header must never
 * redeclare any of it. */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

#define PERMISSION__APPDATA_PATH_MAX 96

/* The app's appdata folder ("appdata" in its /config/permissions.json entry).
 * Returns false when none is saved. Used by core/appdata. */
bool permission__get_appdata(const char *file_name, char *out_path, size_t capacity);
bruce_result_t permission__set_appdata(const char *file_name, const char *path);

/* Clears every in-memory decision and deletes /config/permissions.json so the next
 * permission__check()/permission__preflight() call starts from a clean
 * slate. Returns false only if the file existed and could not be removed. */
bool permission__test_reset(void);
