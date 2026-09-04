#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

/**
 * @brief Programmatic access to the launcher's menu tree.
 *
 * The menu lives as one JSON object at /config/launcher.conf: each key is
 * "Label" or "Label@icon-name" (see core/icon/icon_assets.h for the valid
 * icon names) and each value is either a command string (a builtin command
 * name or an absolute app path) or a nested object describing a submenu -
 * see embedded_resources/json/launcher.json (modules/bruce_launcher) for the
 * shipped default, and modules/bruce_launcher/bruce_launcher_menu.c for how
 * it's loaded. This header only supports appending a flat command entry to
 * the root or one of its immediate submenus - launcher__add_menu_entry()
 * itself asks where with a dialog__choice() prompt, e.g. so a long press
 * elsewhere in the UI can offer "Add to..." without the caller building that
 * prompt itself. Adding/editing submenus themselves, reordering entries, or
 * reaching a submenu nested more than one level deep still requires
 * hand-editing the config file.
 */

#define BRUCE_LAUNCHER_ENTRY_LABEL_MAX 48
#define BRUCE_LAUNCHER_ENTRY_ICON_MAX 32
#define BRUCE_LAUNCHER_ENTRY_COMMAND_MAX 128

/** Label launcher__list_menus() always returns first, for the root menu itself. */
#define BRUCE_LAUNCHER_ROOT_MENU_LABEL "Main Menu"

/* The root can hold at most as many entries as bruce_launcher_menu.c will
 * load (see BRUCE_LAUNCHER_MAX_ENTRIES there) - one destination label per
 * root entry, plus the root itself. */
#define BRUCE_LAUNCHER_MENU_LIST_MAX 33

/**
 * @brief Lists the menus a new entry can be added to.
 *
 * out_labels[0] is always BRUCE_LAUNCHER_ROOT_MENU_LABEL, representing the
 * launcher's root menu; every entry after that is one of the root's own
 * immediate submenus (a top-level entry whose value is a JSON object rather
 * than a command string), labeled with its "Label@icon-name" key's label
 * half. Returns the number of labels written (at least 1, capped at
 * `capacity`), or 0 only if `out_labels`/`capacity` themselves are invalid.
 *
 * @param out_labels Array of caller-owned label buffers to fill.
 * @param capacity Number of entries out_labels can hold.
 */
size_t launcher__list_menus(char out_labels[][BRUCE_LAUNCHER_ENTRY_LABEL_MAX], size_t capacity);

/**
 * @brief Reports whether the given menu already has a command entry equal to `command`.
 *
 * Only looks at flat command entries directly on that menu, not inside its
 * own submenus. Returns false (never an error) when /config/launcher.conf
 * doesn't exist yet, can't be parsed, or `menu_label` doesn't name a menu
 * launcher__list_menus() would list - this is meant to steer what a caller
 * offers (e.g. "Add to WiFi" vs "Already in WiFi"), not to validate the
 * file.
 *
 * @param menu_label Destination menu, as returned by launcher__list_menus(); NULL/"" means the root.
 * @param command Command string to search for (e.g. an app's absolute path).
 */
bool launcher__menu_has_command(const char *menu_label, const char *command);

/**
 * @brief Appends a command entry to the launcher menu tree, asking where.
 *
 * Owns the whole interaction: when /config/launcher.conf has more than one
 * destination (the root plus its own submenus - see launcher__list_menus()),
 * shows a dialog__choice() prompt listing them (with a "Cancel" row) and
 * proceeds with whatever the user picked; with only the root to choose from,
 * adds there directly, no prompt. Also reports the outcome itself via
 * dialog__message() - "Added to X" or "Already in X" (a no-op when `command`
 * is already present on the chosen menu - callers don't need to check first
 * to stay idempotent) - so callers don't have to build any of this
 * themselves; a long press elsewhere in the UI offering "Add to main menu"
 * can just call this and handle the return code.
 *
 * BRUCE_ERR_CANCELLED if the prompt was cancelled (nothing shown beyond the
 * prompt itself, and the file untouched, same as any other dialog__* cancel).
 * BRUCE_ERR_NOT_FOUND if /config/launcher.conf doesn't exist yet (the
 * launcher writes its default the first time it runs, so this is only
 * expected before the launcher has ever started). BRUCE_ERR_RESOURCE_LIMIT
 * if the chosen menu already has as many entries as the launcher will load,
 * or the file would grow past its size limit.
 *
 * @param label Entry label shown in the menu, truncated to BRUCE_LAUNCHER_ENTRY_LABEL_MAX - 1.
 * @param icon_name Optional built-in icon name (core/icon/icon_assets.h), or NULL/"" for none.
 * @param command Command string to run when the entry is picked.
 */
bruce_result_t launcher__add_menu_entry(const char *label, const char *icon_name, const char *command);
