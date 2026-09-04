#pragma once

/* Pure parsing/matching helpers factored out of filemanager_pathicons.c so
 * the selftest module can unit-test them directly
 * (selftest__run_filemanager_pathicons_* cases in
 * modules/selftest/filemanager_pathicons_test.c) without touching
 * app_config/storage. Not part of the public core_sdk/ API: other modules
 * must not include this header, only filemanager_app.h.
 */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/dialog.h"
#include "core_sdk/storage.h"

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];   /* Listing path this entry overrides the icon for, e.g. "/Network". */
    char icon[BRUCE_DIALOG_ICON_NAME_MAX]; /* Built-in icon name (see core_sdk/icon.h). */
} filemanager_pathicons__entry_t;

/* Parses "/config/filemanager.conf"'s "pathicons" JSON -- an array of
 * {"path", "icon"} objects, e.g.
 *   [{"path": "/Network", "icon": "server"}]
 * -- into `entries`, stopping at `max_entries`. Both fields are required;
 * an entry missing either, with an empty value, or with a value too long
 * for its buffer is skipped rather than aborting the whole parse. Returns
 * false (leaving *out_count at 0) only when `json_text` itself isn't
 * parseable JSON shaped like an array at all. */
bool filemanager_pathicons__parse_json(
    const char *json_text, filemanager_pathicons__entry_t *entries, size_t max_entries, size_t *out_count
);

/* True and fills out_icon when `path` exactly matches a configured entry's
 * path (first match wins). False (out_icon untouched) otherwise. Pure
 * lookup over an already-parsed list, split out from
 * filemanager_pathicons__icon_for_path() so selftest can exercise matching
 * without app_config I/O. */
bool filemanager_pathicons__match(
    const filemanager_pathicons__entry_t *entries, size_t entry_count, const char *path, char *out_icon,
    size_t out_icon_size
);
