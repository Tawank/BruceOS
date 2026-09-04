#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "core_sdk/launcher.h"

/* Pulled out of launcher.c so selftest can exercise these without any
 * storage I/O. */

/* Builds the "Label" or "Label@icon-name" key launcher.json uses for a flat
 * command entry. Returns false (leaving out_key untouched) when label is
 * NULL/empty or out_key has no room; truncates a label/icon combination that
 * doesn't fit out_key_size, same as every other fixed-buffer entry field in
 * this codebase. */
bool launcher__build_entry_key(const char *label, const char *icon_name, char *out_key, size_t out_key_size);

/* Splits a JSON key into its label half, undoing launcher__build_entry_key()
 * -- the same "Label" vs "Label@icon-name" split
 * bruce_launcher__parse_label() (modules/bruce_launcher/bruce_launcher_menu.c)
 * uses when loading. Returns false (leaving out_label untouched) for a NULL
 * key or a zero-capacity buffer; truncates a label that doesn't fit. */
bool launcher__label_from_key(const char *key, char *out_label, size_t out_label_size);

/* Reports whether `menu` (a parsed menu object - the root, or one of its
 * submenus) already has a flat command entry - a string value, not a
 * submenu object - equal to `command`. NULL-safe: returns false for a NULL
 * menu or command. */
bool launcher__json_has_command(const cJSON *menu, const char *command);

/* Fills out_labels with the destination labels launcher__list_menus()
 * documents: out_labels[0] is always BRUCE_LAUNCHER_ROOT_MENU_LABEL,
 * followed by launcher__label_from_key() applied to every top-level
 * submenu's key (entries whose value isn't an object are skipped). Returns
 * the number of labels written, capped at `capacity` (at least 1 whenever
 * capacity > 0, even when `root` is NULL/empty). */
size_t
launcher__json_menu_labels(const cJSON *root, char out_labels[][BRUCE_LAUNCHER_ENTRY_LABEL_MAX], size_t capacity);

/* Finds the object node a menu_label (as launcher__list_menus() returns it)
 * refers to: `root` itself when menu_label is NULL/""/
 * BRUCE_LAUNCHER_ROOT_MENU_LABEL, or the top-level submenu whose key's label
 * half matches otherwise. NULL if menu_label names something that isn't one
 * of root's own submenus, or root itself is NULL. */
cJSON *launcher__json_find_menu(cJSON *root, const char *menu_label);
