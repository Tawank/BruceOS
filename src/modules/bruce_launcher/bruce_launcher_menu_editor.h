#pragma once

#include "core_sdk/result.h"

/* GUI tree editor for /config/launcher.conf -- reorder, rename, re-icon,
 * add, delete, and move entries between menus at any depth. Reached from
 * bruce_launcher_config__gui()'s "Menu entries" row (bruce_launcher_config.c);
 * see core_sdk/launcher.h's launcher__tree_*() family for the storage side. */

/* Runs the editor until the user backs all the way out of it (BRUCE_OK), or
 * a real storage failure occurs. GUI only -- callers should only reach this
 * once runtime__gui_requested() is already known true. */
bruce_result_t bruce_launcher_menu_editor__run(void);
