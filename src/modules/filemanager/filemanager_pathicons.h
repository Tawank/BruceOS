#pragma once

/* "pathicons": "/config/filemanager.conf"'s "pathicons" array of
 * {"path", "icon"} objects, mapping a specific listing path (e.g.
 * "/Network") to a built-in icon (core_sdk/icon.h) shown in filemanager's
 * browser instead of the picker's default "folder"/per-extension icon.
 * Seeded with a "/Network" entry the first time "/config/filemanager.conf"
 * gains a "pathicons" key (same "get, and set back if absent" seeding
 * idiom modules/system_menu/system_menu_app.c uses for its own "items"
 * key). Not part of the public core_sdk/ API: other modules must not
 * include this header, only filemanager_app.h.
 */

#include <stdbool.h>
#include <stddef.h>

/* bruce_dialog_render_params_t.icon_for_path-shaped (core_sdk/dialog.h) --
 * filemanager_app.c wires this straight into its main browse loop's
 * dialog__pick_file_ex() call so a configured pathicons entry overrides the
 * picker's default icon for that exact path. `context` is unused. */
bool filemanager_pathicons__icon_for_path(
    const char *path, bool is_directory, char *out_icon, size_t out_icon_size, void *context
);
