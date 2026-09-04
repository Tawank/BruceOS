#pragma once

/* Single-entry file/folder actions (everything on filemanager_app.c's action
 * menus except Copy/Paste -- see filemanager_clipboard.h -- and the
 * "/Network" folder itself -- see filemanager_network.h). Not part of the
 * public core_sdk/ API: other modules must not include this header, only
 * filemanager_app.h.
 */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

/* Creates a new empty file (or, if `folder`, a directory) under `directory`,
 * prompting for its name. */
bruce_result_t filemanager__new_entry(const char *directory, bool folder);

/* Opens `path` with whatever extensions.conf maps its type to, or, when
 * `path` is a "/Network"-discovered location, the location's own provider
 * (see filemanager_network.h) -- foreground, waits for it to exit. */
bruce_result_t filemanager__open_default(const char *path, bool gui);

/* Prompts for one of a fixed set of viewer/editor apps and opens `path` with
 * it, ignoring extensions.conf entirely. */
bruce_result_t filemanager__pick_open_with_app(const char *path, bool gui);

/* Shows up to FILEMANAGER_PREVIEW_MAX bytes of `path` -- a scrollable GUI
 * text viewer, or printed to stdout headless. */
bruce_result_t filemanager__view_file(const char *path, bool gui);

/* Opens `path` in the "text" app for editing. */
bruce_result_t filemanager__edit_file(const char *path, bool gui);

/* Shows `path`'s size in an info dialog. */
bruce_result_t filemanager__show_info(const char *path);

/* Shows `path`'s entry count in an info dialog (the directory counterpart to
 * filemanager__show_info()). */
bruce_result_t filemanager__show_folder_info(const char *path);

/* Prompts for a new name and renames `path` (file or folder) in place; on
 * success rewrites `path` itself to the new full path (`path_size` is its
 * buffer's capacity) so the caller can re-select it. */
bruce_result_t filemanager__rename_entry(char *path, size_t path_size);

/* Confirms, then deletes, `path`. `kind` ("file"/"folder") only changes the
 * confirmation dialog's wording. */
bruce_result_t filemanager__delete_entry(const char *path, const char *kind);
