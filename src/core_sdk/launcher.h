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
 * it's loaded. launcher__add_menu_entry() appends a flat command entry to
 * the root or one of its immediate submenus, asking where with a
 * dialog__choice() prompt, e.g. so a long press elsewhere in the UI can
 * offer "Add to..." without the caller building that prompt itself. The
 * launcher__tree_*() functions below instead address any menu at any depth
 * directly (a "path" of submenu labels from the root) for a full tree
 * editor - see modules/bruce_launcher/bruce_launcher_menu_editor.c, reached
 * from "bruce_launcher config"'s "Menu entries" screen.
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

/* -------------------------------------------------------------------------- */
/* Full tree editor                                                           */
/* -------------------------------------------------------------------------- */

/* Deepest submenu nesting the functions below will walk into - generous
 * relative to how deep anyone actually nests menus; guards the fixed-size
 * path arrays callers pass in. */
#define BRUCE_LAUNCHER_TREE_MAX_DEPTH 8

/* Most entries any single menu (the root or one submenu) can hold - mirrors
 * bruce_launcher_menu.c's own BRUCE_LAUNCHER_MAX_ENTRIES load limit, so a
 * caller never tries to add past what will actually load back. */
#define BRUCE_LAUNCHER_TREE_ENTRIES_MAX 32

typedef struct {
    char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
    char icon_name[BRUCE_LAUNCHER_ENTRY_ICON_MAX];     /* "" if the entry has none. */
    char command[BRUCE_LAUNCHER_ENTRY_COMMAND_MAX];    /* "" for a submenu entry. */
    bool is_submenu;
} bruce_launcher_tree_entry_t;

/**
 * @brief Lists one menu's entries, in on-disk (and on-screen) order.
 *
 * Every function in this group addresses a menu the same way:
 * `path[0..path_depth)` is a chain of submenu labels, each an immediate
 * child of the previous, walked from /config/launcher.conf's root;
 * `path_depth == 0` means the root menu itself.
 *
 * @param path Submenu labels from the root, or NULL when path_depth is 0.
 * @param path_depth Number of labels in path (0..BRUCE_LAUNCHER_TREE_MAX_DEPTH).
 * @param out_entries Array of caller-owned entries to fill.
 * @param capacity Number of entries out_entries can hold.
 * @return Number of entries written, capped at `capacity`; 0 if `path` doesn't resolve to a menu.
 */
size_t launcher__tree_list(
    const char *const *path, size_t path_depth, bruce_launcher_tree_entry_t *out_entries, size_t capacity
);

/**
 * @brief Moves the entry at `index` one slot within its own menu.
 *
 * @param path Menu's path from the root; see launcher__tree_list().
 * @param path_depth Number of labels in path.
 * @param index Entry to move.
 * @param direction -1 moves it up (earlier), +1 moves it down (later).
 * @return BRUCE_ERR_INVALID_ARGUMENT if `index` is already at that edge or out of range.
 */
bruce_result_t launcher__tree_move(const char *const *path, size_t path_depth, size_t index, int direction);

/**
 * @brief Deletes the entry at `index` - if it's a submenu, its whole subtree goes with it.
 *
 * @param path Menu's path from the root; see launcher__tree_list().
 * @param path_depth Number of labels in path.
 * @param index Entry to delete.
 */
bruce_result_t launcher__tree_delete(const char *const *path, size_t path_depth, size_t index);

/**
 * @brief Renames the entry at `index`, leaving its icon/kind/value unchanged.
 *
 * @param path Menu's path from the root; see launcher__tree_list().
 * @param path_depth Number of labels in path.
 * @param index Entry to rename.
 * @param new_label New label, truncated to BRUCE_LAUNCHER_ENTRY_LABEL_MAX - 1.
 */
bruce_result_t launcher__tree_rename(const char *const *path, size_t path_depth, size_t index, const char *new_label);

/**
 * @brief Sets (or, with NULL/"", clears) the icon shown for the entry at `index`.
 *
 * @param path Menu's path from the root; see launcher__tree_list().
 * @param path_depth Number of labels in path.
 * @param index Entry to update.
 * @param icon_name Built-in icon name (core/icon/icon_assets.h), or NULL/"" to clear it.
 */
bruce_result_t
launcher__tree_set_icon(const char *const *path, size_t path_depth, size_t index, const char *icon_name);

/**
 * @brief Appends a new command entry directly to the menu at `path`.
 *
 * Unlike launcher__add_menu_entry(), never prompts for a destination - the
 * caller has already picked one by way of `path`.
 *
 * @param path Destination menu's path from the root; see launcher__tree_list().
 * @param path_depth Number of labels in path.
 * @param label Entry label, truncated to BRUCE_LAUNCHER_ENTRY_LABEL_MAX - 1.
 * @param icon_name Optional built-in icon name, or NULL/"" for none.
 * @param command Command string to run when the entry is picked.
 * @return BRUCE_ERR_RESOURCE_LIMIT if the menu already has BRUCE_LAUNCHER_TREE_ENTRIES_MAX entries.
 */
bruce_result_t launcher__tree_add_command(
    const char *const *path, size_t path_depth, const char *label, const char *icon_name, const char *command
);

/**
 * @brief Appends a new, empty submenu directly to the menu at `path`.
 *
 * @param path Destination menu's path from the root; see launcher__tree_list().
 * @param path_depth Number of labels in path.
 * @param label Submenu label, truncated to BRUCE_LAUNCHER_ENTRY_LABEL_MAX - 1.
 * @param icon_name Optional built-in icon name, or NULL/"" for none.
 * @return BRUCE_ERR_RESOURCE_LIMIT if the menu already has BRUCE_LAUNCHER_TREE_ENTRIES_MAX entries.
 */
bruce_result_t
launcher__tree_add_submenu(const char *const *path, size_t path_depth, const char *label, const char *icon_name);

/**
 * @brief Moves the entry at `index` in the menu at `path` to become the last entry of `dest_path`.
 *
 * Moves the entry's whole subtree along with it when it's a submenu. A
 * no-op (BRUCE_OK, nothing changed) when `dest_path` names the same menu the
 * entry is already in.
 *
 * @param path Source menu's path from the root; see launcher__tree_list().
 * @param path_depth Number of labels in path.
 * @param index Entry to move.
 * @param dest_path Destination menu's path from the root.
 * @param dest_path_depth Number of labels in dest_path.
 * @return BRUCE_ERR_ALREADY_EXISTS if the destination menu already has an entry with the same label.
 * BRUCE_ERR_INVALID_ARGUMENT if `dest_path` is the moved entry itself or nested inside it (which would
 * disconnect it from the tree).
 */
bruce_result_t launcher__tree_move_to(
    const char *const *path, size_t path_depth, size_t index, const char *const *dest_path, size_t dest_path_depth
);
