#pragma once

/* Copy/Paste actions (see filemanager_app.c's action menus). Not part of the
 * public core_sdk/ API: other modules must not include this header, only
 * filemanager_app.h.
 */

#include "core_sdk/result.h"

/* "Copy": adds `path` (a file or folder) to the shared clipboard so a later
 * "Paste" here, in a different directory, or in another app entirely, can
 * paste it -- see core_sdk/clipboard.h. */
bruce_result_t filemanager__copy_entry(const char *path);

/* "Paste": pastes the clipboard's file(s)/folder(s), or binary payload, into
 * `directory`. Reports BRUCE_ERR_INVALID_STATE if the clipboard holds
 * neither (e.g. it's empty, or holds text copied by some other app). */
bruce_result_t filemanager__paste_here(const char *directory);
