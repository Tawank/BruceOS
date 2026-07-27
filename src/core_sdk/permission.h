#pragma once

/*
 * Coarse-grained app permission model (public SDK surface).
 *
 * Every ELF/JS app is identified, for permission purposes, by its filename
 * including extension and without its path (e.g. "game.elf", "weather.js").
 * Decisions are persisted in Core-owned /permissions.json, keyed by that
 * filename; apps sharing a basename deliberately share the same decision.
 * Built-in modules are implicitly granted every permission and never
 * consult the saved-decision store.
 */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

#define BRUCE_PERMISSION_FILE_NAME_MAX 48

typedef enum {
    BRUCE_PERMISSION_HTTP = 0,
    BRUCE_PERMISSION_WIFI,
    BRUCE_PERMISSION_BT,
    BRUCE_PERMISSION_GPS,
    BRUCE_PERMISSION_RF,
    BRUCE_PERMISSION_INPUT,
    BRUCE_PERMISSION_GPIO,
    BRUCE_PERMISSION_IR,
    BRUCE_PERMISSION_RFID,
    BRUCE_PERMISSION_MICROPHONE,
    BRUCE_PERMISSION_HID,
    BRUCE_PERMISSION_EXECUTE,
    BRUCE_PERMISSION_TASK,
    BRUCE_PERMISSION_STORAGE,
    BRUCE_PERMISSION_CONFIG,
    BRUCE_PERMISSION_SERIAL,
    BRUCE_PERMISSION_COUNT,
} bruce_permission_t;

/* Returns the canonical lowercase name ("wifi", "http", ...) used in
 * manifests and /permissions.json, or NULL for an out-of-range value. */
const char *permission__name(bruce_permission_t permission);

/* Resolves a manifest/JSON permission name to its enum value. Returns false
 * (leaving *out_permission untouched) for an unknown name. */
bool permission__from_name(const char *name, bruce_permission_t *out_permission);

/* Checks whether the *calling* task currently holds `permission`. This is
 * the function every protected Core API (wifi__*, http__*, storage__*,
 * config__*, task__* control of another task, app_runner__run, ...) calls
 * internally; app/module code never needs to call it directly.
 *
 * A built-in task always returns BRUCE_OK. An external (ELF/JS) task with an
 * existing saved decision returns immediately (BRUCE_OK or
 * BRUCE_ERR_PERMISSION) with no prompt. With no saved decision yet, this is
 * the dynamic first-use request: it shows an allow/deny dialog__choice(),
 * persists the answer keyed by the task's permission file name, and returns
 * accordingly. If the dialog itself fails (e.g. is cancelled) the decision
 * is left unresolved (not persisted) and BRUCE_ERR_PERMISSION is returned,
 * so a later call may prompt again. Returns BRUCE_ERR_INVALID_ARGUMENT for
 * an out-of-range `permission`. */
bruce_result_t permission__check(bruce_permission_t permission);

/* Pre-launch batch request: for every name in `permission_names` that
 * `file_name` has no saved decision for yet, prompts (unchecked/undecided by
 * default) and persists the user's choice; already-known permissions are
 * left untouched and not re-prompted. Intended for the ELF/JS loaders
 * (Stage 3) to call with the manifest's declared permission list before a
 * new task's first instruction runs. Returns BRUCE_OK once every name has
 * been processed (regardless of individual allow/deny outcomes) or
 * BRUCE_ERR_INVALID_ARGUMENT for an invalid `file_name` or an unknown
 * permission name. */
bruce_result_t
permission__preflight(const char *file_name, const char *const *permission_names, size_t count);

/* Returns the saved decision without prompting, e.g. for a
 * permissions-management UI. Returns BRUCE_ERR_NOT_FOUND if `file_name` has
 * no saved decision yet for `permission`. */
bruce_result_t permission__get_saved(const char *file_name, bruce_permission_t permission, bool *out_allowed);

/* Persists an explicit allow/deny decision for `file_name`, e.g. from a
 * permissions-management UI. */
bruce_result_t permission__set(const char *file_name, bruce_permission_t permission, bool allowed);
