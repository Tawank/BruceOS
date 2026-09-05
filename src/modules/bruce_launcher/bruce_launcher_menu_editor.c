#include "bruce_launcher_menu_editor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/dialog.h"
#include "core_sdk/icon.h"
#include "core_sdk/launcher.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"

/* A menu's address in the tree, as launcher__tree_*() expects it: a chain
 * of submenu labels from the root, root itself when depth == 0. */
typedef struct {
    char labels[BRUCE_LAUNCHER_TREE_MAX_DEPTH][BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
    size_t depth;
} bruce_launcher_menu_editor__path_t;

/* The entry a pending "Move to..." is relocating, and where it started --
 * active == false means no move is in progress and the browser behaves
 * normally. */
typedef struct {
    bool active;
    bruce_launcher_menu_editor__path_t path;
    size_t index;
    char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
} bruce_launcher_menu_editor__move_t;

static void bruce_launcher_menu_editor__path_ptrs(
    const bruce_launcher_menu_editor__path_t *path, const char *out[BRUCE_LAUNCHER_TREE_MAX_DEPTH]
) {
    for (size_t i = 0; i < path->depth; ++i) out[i] = path->labels[i];
}

static bruce_result_t bruce_launcher_menu_editor__add_command(const char *const *path_ptrs, size_t depth) {
    char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX] = {0};
    if (dialog__text_input("Add entry", "Label", "", false, label, sizeof(label)) != BRUCE_OK || label[0] == '\0') {
        return BRUCE_ERR_CANCELLED;
    }
    char command[BRUCE_LAUNCHER_ENTRY_COMMAND_MAX] = {0};
    if (dialog__text_input("Add entry", "Command to launch", "", false, command, sizeof(command)) != BRUCE_OK ||
        command[0] == '\0') {
        return BRUCE_ERR_CANCELLED;
    }
    char icon_name[BRUCE_ICON_NAME_MAX] = {0};
    (void)icon__pick("Icon (optional)", NULL, true, icon_name, sizeof(icon_name));
    return launcher__tree_add_command(path_ptrs, depth, label, icon_name, command);
}

static bruce_result_t bruce_launcher_menu_editor__add_submenu(const char *const *path_ptrs, size_t depth) {
    char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX] = {0};
    if (dialog__text_input("Add submenu", "Label", "", false, label, sizeof(label)) != BRUCE_OK ||
        label[0] == '\0') {
        return BRUCE_ERR_CANCELLED;
    }
    char icon_name[BRUCE_ICON_NAME_MAX] = {0};
    (void)icon__pick("Icon (optional)", NULL, true, icon_name, sizeof(icon_name));
    return launcher__tree_add_submenu(path_ptrs, depth, label, icon_name);
}

/* Per-entry action sheet: reorder/rename/re-icon/delete it, start a "Move
 * to..." pick, open it (submenus only), or back out unchanged -- mirrors
 * config_app.c's per-item sheet (config_app__startup_item_gui). Every action
 * but "Move to..."/"Open" is applied directly here; setting *out_start_move
 * or *out_open leaves the caller to act on it, since both reuse the same
 * browsing loop (picking a destination, or descending a level).
 * *out_next_selected reports which row should stay highlighted next time
 * this same menu is drawn -- index unless an up/down move changed it. */
static bruce_result_t bruce_launcher_menu_editor__entry_actions(
    const bruce_launcher_menu_editor__path_t *path, size_t index, const bruce_launcher_tree_entry_t *entry,
    size_t entry_count, bool *out_start_move, bool *out_open, size_t *out_next_selected
) {
    *out_start_move = false;
    *out_open = false;
    *out_next_selected = index;
    const char *path_ptrs[BRUCE_LAUNCHER_TREE_MAX_DEPTH];
    bruce_launcher_menu_editor__path_ptrs(path, path_ptrs);

    bruce_dialog_choice_t choices[8];
    size_t n = 0;
    size_t open_index = n;
    if (entry->is_submenu) choices[n++] = (bruce_dialog_choice_t){.label = "Open", .value = "open"};
    bool has_up = index > 0;
    bool has_down = index + 1 < entry_count;
    size_t up_index = n;
    if (has_up) choices[n++] = (bruce_dialog_choice_t){.label = "Move up", .value = "up"};
    size_t down_index = n;
    if (has_down) choices[n++] = (bruce_dialog_choice_t){.label = "Move down", .value = "down"};
    size_t rename_index = n;
    choices[n++] = (bruce_dialog_choice_t){.label = "Rename", .value = "rename"};
    size_t icon_index = n;
    choices[n++] = (bruce_dialog_choice_t){.label = "Change icon", .value = "icon"};
    size_t move_to_index = n;
    choices[n++] = (bruce_dialog_choice_t){.label = "Move to...", .value = "move_to"};
    size_t delete_index = n;
    choices[n++] = (bruce_dialog_choice_t){.label = "Delete", .value = "delete"};
    size_t back_index = n;
    choices[n++] = (bruce_dialog_choice_t){.label = "Back", .value = "back"};

    size_t selected = 0;
    bruce_result_t result = dialog__choice_launcher(entry->label, NULL, choices, n, &selected);
    if (result != BRUCE_OK || selected == back_index) return BRUCE_OK;

    if (entry->is_submenu && selected == open_index) {
        *out_open = true;
        return BRUCE_OK;
    }
    if (has_up && selected == up_index) {
        *out_next_selected = index - 1;
        return launcher__tree_move(path_ptrs, path->depth, index, -1);
    }
    if (has_down && selected == down_index) {
        *out_next_selected = index + 1;
        return launcher__tree_move(path_ptrs, path->depth, index, 1);
    }

    if (selected == rename_index) {
        char new_label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX] = {0};
        if (dialog__text_input("Rename", "Entry label", entry->label, false, new_label, sizeof(new_label)) !=
                BRUCE_OK ||
            new_label[0] == '\0') {
            return BRUCE_OK;
        }
        return launcher__tree_rename(path_ptrs, path->depth, index, new_label);
    }
    if (selected == icon_index) {
        char icon_name[BRUCE_ICON_NAME_MAX] = {0};
        if (icon__pick("Change icon", entry->icon_name, true, icon_name, sizeof(icon_name)) != BRUCE_OK) {
            return BRUCE_OK;
        }
        return launcher__tree_set_icon(path_ptrs, path->depth, index, icon_name);
    }
    if (selected == move_to_index) {
        *out_start_move = true;
        return BRUCE_OK;
    }
    if (selected == delete_index) {
        char message[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 32];
        if (entry->is_submenu) snprintf(message, sizeof(message), "Delete \"%s\" and everything in it?", entry->label);
        else snprintf(message, sizeof(message), "Delete \"%s\"?", entry->label);
        bruce_dialog_choice_t confirm_choices[2] = {
            {.label = "Delete", .value = "delete"},
            {.label = "Cancel", .value = "cancel"},
        };
        size_t confirm_selected = 1;
        bruce_result_t confirm_result =
            dialog__choice_launcher("Delete entry", message, confirm_choices, 2, &confirm_selected);
        if (confirm_result != BRUCE_OK || confirm_selected != 0) return BRUCE_OK;
        return launcher__tree_delete(path_ptrs, path->depth, index);
    }
    return BRUCE_OK;
}

bruce_result_t bruce_launcher_menu_editor__run(void) {
    /* Heap-allocated, not plain locals: together path and move are ~800
     * bytes, and this function already runs deep under bruce_launcher's own
     * default-stack process (bruce_launcher_app_main -> ...__config__run ->
     * ...__config__gui -> here), itself several frames beneath whatever
     * Core dialog/display rendering the choice list below descends into.
     * That combination overflowed the task's stack the first time this
     * screen shipped; see launcher__prompt_menu()'s comment (launcher.c) for
     * the same fix applied to an earlier instance of this exact problem. */
    bruce_launcher_menu_editor__path_t *path = memory__calloc(1, sizeof(*path));
    bruce_launcher_menu_editor__move_t *move = memory__calloc(1, sizeof(*move));
    if (path == NULL || move == NULL) {
        memory__free(path);
        memory__free(move);
        return BRUCE_ERR_NO_MEMORY;
    }

    /* Row remembered per tree depth, so acting on an entry (move up/down,
     * rename, ...) or backing out of a submenu redraws that same list with
     * the same row highlighted instead of always snapping back to the top. */
    size_t selected_at_depth[BRUCE_LAUNCHER_TREE_MAX_DEPTH + 1] = {0};

    for (;;) {
        const char *path_ptrs[BRUCE_LAUNCHER_TREE_MAX_DEPTH];
        bruce_launcher_menu_editor__path_ptrs(path, path_ptrs);

        bruce_launcher_tree_entry_t *entries =
            memory__calloc(BRUCE_LAUNCHER_TREE_ENTRIES_MAX, sizeof(*entries));
        if (entries == NULL) {
            memory__free(path);
            memory__free(move);
            return BRUCE_ERR_NO_MEMORY;
        }
        size_t count = launcher__tree_list(path_ptrs, path->depth, entries, BRUCE_LAUNCHER_TREE_ENTRIES_MAX);

        size_t capacity = count + 2 + 1; /* entries + up to two control rows + Back */
        bruce_dialog_choice_t *choices = memory__calloc(capacity, sizeof(*choices));
        if (choices == NULL) {
            memory__free(entries);
            memory__free(path);
            memory__free(move);
            return BRUCE_ERR_NO_MEMORY;
        }
        size_t n = 0;
        for (; n < count; ++n) {
            choices[n] = (bruce_dialog_choice_t){
                .label = entries[n].label,
                .value = entries[n].label,
                .icon_name = entries[n].icon_name[0] != '\0' ? entries[n].icon_name : NULL,
            };
        }
        size_t move_here_index = n;
        size_t add_command_index = n;
        size_t add_submenu_index = n;
        if (move->active) {
            choices[n++] = (bruce_dialog_choice_t){.label = "Move here", .value = "move_here"};
        } else if (count < BRUCE_LAUNCHER_TREE_ENTRIES_MAX) {
            add_command_index = n;
            choices[n++] = (bruce_dialog_choice_t){.label = "Add entry", .value = "add_command"};
            add_submenu_index = n;
            choices[n++] = (bruce_dialog_choice_t){.label = "Add submenu", .value = "add_submenu"};
        }
        size_t back_index = n;
        choices[n++] = (bruce_dialog_choice_t){.label = "Back", .value = "back"};

        char title[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 16];
        if (move->active) snprintf(title, sizeof(title), "Move \"%s\"", move->label);
        else if (path->depth == 0) snprintf(title, sizeof(title), "Menu entries");
        else snprintf(title, sizeof(title), "%s", path->labels[path->depth - 1]);

        size_t depth = path->depth;
        if (selected_at_depth[depth] >= n) selected_at_depth[depth] = n > 0 ? n - 1 : 0;
        size_t selected = selected_at_depth[depth];
        bruce_result_t result = dialog__choice_launcher(title, NULL, choices, n, &selected);
        selected_at_depth[depth] = selected;
        memory__free(choices);

        bool back = result == BRUCE_ERR_CANCELLED || (result == BRUCE_OK && selected == back_index);
        if (back) {
            memory__free(entries);
            if (move->active && path->depth == 0) {
                move->active = false;
                continue;
            }
            if (path->depth == 0) {
                memory__free(path);
                memory__free(move);
                return BRUCE_OK;
            }
            path->depth--;
            continue;
        }
        if (result != BRUCE_OK) {
            memory__free(entries);
            continue;
        }

        if (move->active && selected == move_here_index) {
            const char *src_ptrs[BRUCE_LAUNCHER_TREE_MAX_DEPTH];
            bruce_launcher_menu_editor__path_ptrs(&move->path, src_ptrs);
            bruce_result_t move_result =
                launcher__tree_move_to(src_ptrs, move->path.depth, move->index, path_ptrs, path->depth);
            move->active = false;
            memory__free(entries);
            if (move_result == BRUCE_ERR_ALREADY_EXISTS) {
                (void)dialog__message(BRUCE_DIALOG_ERROR, "Move", "An entry with that name already exists there");
            } else if (move_result != BRUCE_OK) {
                (void)dialog__message(BRUCE_DIALOG_ERROR, "Move", "Couldn't move that entry there");
            } else {
                selected_at_depth[path->depth] = count; /* highlight the entry that just landed here */
            }
            continue;
        }
        if (!move->active && selected == add_command_index) {
            bruce_result_t add_result = bruce_launcher_menu_editor__add_command(path_ptrs, path->depth);
            memory__free(entries);
            if (add_result == BRUCE_ERR_RESOURCE_LIMIT) {
                (void)dialog__message(BRUCE_DIALOG_ERROR, "Add entry", "This menu is full");
            } else if (add_result == BRUCE_OK) {
                selected_at_depth[path->depth] = count; /* highlight the newly added entry */
            } else if (add_result != BRUCE_ERR_CANCELLED) {
                memory__free(path);
                memory__free(move);
                return add_result;
            }
            continue;
        }
        if (!move->active && selected == add_submenu_index) {
            bruce_result_t add_result = bruce_launcher_menu_editor__add_submenu(path_ptrs, path->depth);
            memory__free(entries);
            if (add_result == BRUCE_ERR_RESOURCE_LIMIT) {
                (void)dialog__message(BRUCE_DIALOG_ERROR, "Add submenu", "This menu is full");
            } else if (add_result == BRUCE_OK) {
                selected_at_depth[path->depth] = count; /* highlight the newly added entry */
            } else if (add_result != BRUCE_ERR_CANCELLED) {
                memory__free(path);
                memory__free(move);
                return add_result;
            }
            continue;
        }

        /* Otherwise an actual entry row was picked. `entry` aliases straight
         * into `entries` (freed once we're done with it below) rather than
         * copying the whole ~200-byte struct onto this frame. */
        size_t index = selected;
        const bruce_launcher_tree_entry_t *entry = &entries[index];
        if (move->active) {
            if (entry->is_submenu) {
                /* Browsing for a move destination: descend straight in, same
                 * as every entry used to behave before entries got their own
                 * action sheet below -- there's nothing to edit while picking
                 * where to drop something. */
                if (path->depth < BRUCE_LAUNCHER_TREE_MAX_DEPTH) {
                    snprintf(path->labels[path->depth], BRUCE_LAUNCHER_ENTRY_LABEL_MAX, "%s", entry->label);
                    path->depth++;
                }
            }
            memory__free(entries); /* leaf entries aren't valid move destinations, ignored */
            continue;
        }

        bool start_move = false;
        bool open_submenu = false;
        size_t next_selected = index;
        bruce_result_t action_result = bruce_launcher_menu_editor__entry_actions(
            path, index, entry, count, &start_move, &open_submenu, &next_selected
        );
        if (action_result == BRUCE_OK && open_submenu) {
            if (path->depth < BRUCE_LAUNCHER_TREE_MAX_DEPTH) {
                snprintf(path->labels[path->depth], BRUCE_LAUNCHER_ENTRY_LABEL_MAX, "%s", entry->label);
                path->depth++;
            }
        } else if (action_result == BRUCE_OK && start_move) {
            move->active = true;
            move->path = *path;
            move->index = index;
            snprintf(move->label, sizeof(move->label), "%s", entry->label);
        } else if (action_result == BRUCE_OK) {
            selected_at_depth[path->depth] = next_selected;
        }
        memory__free(entries);
        if (action_result != BRUCE_OK) {
            memory__free(path);
            memory__free(move);
            return action_result;
        }
    }
}
