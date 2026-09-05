#include "core_sdk/launcher.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "launcher_internal.h"

/* Full-tree editor backing modules/bruce_launcher/bruce_launcher_menu_editor.c
 * -- unlike launcher.c's launcher__add_menu_entry() (root or one immediate
 * submenu, and it picks where itself), every function here addresses one
 * menu at any depth directly via a caller-supplied path, and never prompts.
 */

/* Renames `item`'s own JSON key in place, keeping its position among its
 * siblings (a detach-and-readd would move it to the end instead). cJSON's
 * struct fields are public exactly so callers can do this; `item->string`
 * is heap memory it owns via the same allocator cJSON_malloc()/cJSON_free()
 * expose. Returns false (item left unchanged) only on allocation failure. */
static bool launcher__tree_rekey(cJSON *item, const char *new_key) {
    size_t length = strlen(new_key) + 1;
    char *copy = cJSON_malloc(length);
    if (copy == NULL) return false;
    memcpy(copy, new_key, length);
    if (item->string != NULL && !(item->type & cJSON_StringIsConst)) cJSON_free(item->string);
    item->string = copy;
    item->type &= ~cJSON_StringIsConst;
    return true;
}

/* The entry at `index` within `menu`, or NULL if `menu` is NULL or index is
 * out of range. */
static cJSON *launcher__tree_item_at(cJSON *menu, size_t index) {
    if (menu == NULL || index > (size_t)INT_MAX) return NULL;
    return cJSON_GetArrayItem(menu, (int)index);
}

size_t launcher__tree_list(
    const char *const *path, size_t path_depth, bruce_launcher_tree_entry_t *out_entries, size_t capacity
) {
    if (out_entries == NULL || capacity == 0 || path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH) return 0;
    cJSON *root = launcher__load_root();
    cJSON *menu = launcher__json_find_menu_at_path(root, path, path_depth);
    size_t count = 0;
    const cJSON *child;
    cJSON_ArrayForEach(child, menu) {
        if (count >= capacity) break;
        if (child->string == NULL) continue;
        bruce_launcher_tree_entry_t *entry = &out_entries[count];
        memset(entry, 0, sizeof(*entry));
        launcher__label_from_key(child->string, entry->label, sizeof(entry->label));
        launcher__icon_from_key(child->string, entry->icon_name, sizeof(entry->icon_name));
        entry->is_submenu = cJSON_IsObject(child);
        if (!entry->is_submenu && cJSON_IsString(child) && child->valuestring != NULL) {
            snprintf(entry->command, sizeof(entry->command), "%s", child->valuestring);
        }
        count++;
    }
    cJSON_Delete(root);
    return count;
}

bruce_result_t launcher__tree_move(const char *const *path, size_t path_depth, size_t index, int direction) {
    if (path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH || (direction != -1 && direction != 1)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;
    cJSON *menu = launcher__json_find_menu_at_path(root, path, path_depth);
    if (menu == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NOT_FOUND;
    }

    int count = cJSON_GetArraySize(menu);
    long new_index = (long)index + direction;
    if (index >= (size_t)count || new_index < 0 || new_index >= count) {
        cJSON_Delete(root);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    cJSON *item = cJSON_DetachItemFromArray(menu, (int)index);
    if (item == NULL || !cJSON_InsertItemInArray(menu, (int)new_index, item)) {
        cJSON_Delete(root);
        return BRUCE_ERR_INTERNAL;
    }

    bruce_result_t result = launcher__save_root(root);
    cJSON_Delete(root);
    return result;
}

bruce_result_t launcher__tree_delete(const char *const *path, size_t path_depth, size_t index) {
    if (path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH) return BRUCE_ERR_INVALID_ARGUMENT;
    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;
    cJSON *menu = launcher__json_find_menu_at_path(root, path, path_depth);
    if (menu == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NOT_FOUND;
    }
    if (index >= (size_t)cJSON_GetArraySize(menu)) {
        cJSON_Delete(root);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    cJSON_DeleteItemFromArray(menu, (int)index);
    bruce_result_t result = launcher__save_root(root);
    cJSON_Delete(root);
    return result;
}

bruce_result_t launcher__tree_rename(const char *const *path, size_t path_depth, size_t index, const char *new_label) {
    if (new_label == NULL || new_label[0] == '\0' || path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;
    cJSON *menu = launcher__json_find_menu_at_path(root, path, path_depth);
    cJSON *item = launcher__tree_item_at(menu, index);
    if (item == NULL) {
        cJSON_Delete(root);
        return menu == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_INVALID_ARGUMENT;
    }

    char icon[BRUCE_LAUNCHER_ENTRY_ICON_MAX];
    launcher__icon_from_key(item->string, icon, sizeof(icon));
    char key[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 1 + BRUCE_LAUNCHER_ENTRY_ICON_MAX];
    if (!launcher__build_entry_key(new_label, icon, key, sizeof(key)) || !launcher__tree_rekey(item, key)) {
        cJSON_Delete(root);
        return BRUCE_ERR_NO_MEMORY;
    }

    bruce_result_t result = launcher__save_root(root);
    cJSON_Delete(root);
    return result;
}

bruce_result_t
launcher__tree_set_icon(const char *const *path, size_t path_depth, size_t index, const char *icon_name) {
    if (path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH) return BRUCE_ERR_INVALID_ARGUMENT;
    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;
    cJSON *menu = launcher__json_find_menu_at_path(root, path, path_depth);
    cJSON *item = launcher__tree_item_at(menu, index);
    if (item == NULL) {
        cJSON_Delete(root);
        return menu == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_INVALID_ARGUMENT;
    }

    char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
    launcher__label_from_key(item->string, label, sizeof(label));
    char key[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 1 + BRUCE_LAUNCHER_ENTRY_ICON_MAX];
    if (!launcher__build_entry_key(label, icon_name, key, sizeof(key)) || !launcher__tree_rekey(item, key)) {
        cJSON_Delete(root);
        return BRUCE_ERR_NO_MEMORY;
    }

    bruce_result_t result = launcher__save_root(root);
    cJSON_Delete(root);
    return result;
}

bruce_result_t launcher__tree_add_command(
    const char *const *path, size_t path_depth, const char *label, const char *icon_name, const char *command
) {
    if (label == NULL || label[0] == '\0' || command == NULL || command[0] == '\0' ||
        path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;
    cJSON *menu = launcher__json_find_menu_at_path(root, path, path_depth);
    if (menu == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NOT_FOUND;
    }
    if (cJSON_GetArraySize(menu) >= LAUNCHER__MAX_ENTRIES) {
        cJSON_Delete(root);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    char key[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 1 + BRUCE_LAUNCHER_ENTRY_ICON_MAX];
    if (!launcher__build_entry_key(label, icon_name, key, sizeof(key)) ||
        cJSON_AddStringToObject(menu, key, command) == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NO_MEMORY;
    }

    bruce_result_t result = launcher__save_root(root);
    cJSON_Delete(root);
    return result;
}

bruce_result_t
launcher__tree_add_submenu(const char *const *path, size_t path_depth, const char *label, const char *icon_name) {
    if (label == NULL || label[0] == '\0' || path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;
    cJSON *menu = launcher__json_find_menu_at_path(root, path, path_depth);
    if (menu == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NOT_FOUND;
    }
    if (cJSON_GetArraySize(menu) >= LAUNCHER__MAX_ENTRIES) {
        cJSON_Delete(root);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    char key[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 1 + BRUCE_LAUNCHER_ENTRY_ICON_MAX];
    if (!launcher__build_entry_key(label, icon_name, key, sizeof(key)) ||
        cJSON_AddObjectToObject(menu, key) == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NO_MEMORY;
    }

    bruce_result_t result = launcher__save_root(root);
    cJSON_Delete(root);
    return result;
}

bruce_result_t launcher__tree_move_to(
    const char *const *path, size_t path_depth, size_t index, const char *const *dest_path, size_t dest_path_depth
) {
    if (path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH || dest_path_depth > BRUCE_LAUNCHER_TREE_MAX_DEPTH) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;
    cJSON *src_menu = launcher__json_find_menu_at_path(root, path, path_depth);
    cJSON *item = launcher__tree_item_at(src_menu, index);
    if (item == NULL) {
        cJSON_Delete(root);
        return src_menu == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_INVALID_ARGUMENT;
    }

    char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
    launcher__label_from_key(item->string, label, sizeof(label));

    /* Reject moving a submenu into itself or one of its own descendants --
     * dest_path can only reach into that subtree by sharing path's own
     * prefix and then continuing with this entry's own label. */
    if (cJSON_IsObject(item)) {
        bool inside_subtree =
            dest_path_depth > path_depth && strcmp(dest_path[path_depth], label) == 0;
        for (size_t i = 0; inside_subtree && i < path_depth; ++i) {
            if (strcmp(dest_path[i], path[i]) != 0) inside_subtree = false;
        }
        if (inside_subtree) {
            cJSON_Delete(root);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    }

    cJSON *dest_menu = launcher__json_find_menu_at_path(root, dest_path, dest_path_depth);
    if (dest_menu == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NOT_FOUND;
    }
    if (dest_menu == src_menu) {
        cJSON_Delete(root);
        return BRUCE_OK;
    }

    const cJSON *existing;
    cJSON_ArrayForEach(existing, dest_menu) {
        char existing_label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
        if (existing->string != NULL &&
            launcher__label_from_key(existing->string, existing_label, sizeof(existing_label)) &&
            strcmp(existing_label, label) == 0) {
            cJSON_Delete(root);
            return BRUCE_ERR_ALREADY_EXISTS;
        }
    }

    cJSON *detached = cJSON_DetachItemViaPointer(src_menu, item);
    if (detached == NULL || !cJSON_AddItemToArray(dest_menu, detached)) {
        cJSON_Delete(root);
        return BRUCE_ERR_INTERNAL;
    }

    bruce_result_t result = launcher__save_root(root);
    cJSON_Delete(root);
    return result;
}
