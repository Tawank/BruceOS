#include "core_sdk/launcher.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "core/storage/storage.h"
#include "core_sdk/dialog.h"
#include "core_sdk/memory.h"
#include "launcher_internal.h"

bool launcher__build_entry_key(const char *label, const char *icon_name, char *out_key, size_t out_key_size) {
    if (label == NULL || label[0] == '\0' || out_key == NULL || out_key_size == 0) return false;
    if (icon_name != NULL && icon_name[0] != '\0') {
        snprintf(out_key, out_key_size, "%s@%s", label, icon_name);
    } else {
        snprintf(out_key, out_key_size, "%s", label);
    }
    return true;
}

bool launcher__label_from_key(const char *key, char *out_label, size_t out_label_size) {
    if (key == NULL || out_label == NULL || out_label_size == 0) return false;
    const char *separator = strrchr(key, '@');
    size_t length = separator != NULL ? (size_t)(separator - key) : strlen(key);
    if (length >= out_label_size) length = out_label_size - 1;
    memcpy(out_label, key, length);
    out_label[length] = '\0';
    return true;
}

bool launcher__icon_from_key(const char *key, char *out_icon, size_t out_icon_size) {
    if (key == NULL || out_icon == NULL || out_icon_size == 0) return false;
    const char *separator = strrchr(key, '@');
    snprintf(out_icon, out_icon_size, "%s", separator != NULL ? separator + 1 : "");
    return true;
}

bool launcher__json_has_command(const cJSON *menu, const char *command) {
    if (menu == NULL || command == NULL) return false;
    const cJSON *child;
    cJSON_ArrayForEach(child, menu) {
        if (cJSON_IsString(child) && child->valuestring != NULL && strcmp(child->valuestring, command) == 0) {
            return true;
        }
    }
    return false;
}

size_t launcher__json_menu_labels(
    const cJSON *root, char out_labels[][BRUCE_LAUNCHER_ENTRY_LABEL_MAX], size_t capacity
) {
    if (out_labels == NULL || capacity == 0) return 0;
    size_t count = 0;
    snprintf(out_labels[count], BRUCE_LAUNCHER_ENTRY_LABEL_MAX, "%s", BRUCE_LAUNCHER_ROOT_MENU_LABEL);
    count++;

    const cJSON *child;
    cJSON_ArrayForEach(child, root) {
        if (count >= capacity) break;
        if (!cJSON_IsObject(child) || child->string == NULL) continue;
        if (launcher__label_from_key(child->string, out_labels[count], BRUCE_LAUNCHER_ENTRY_LABEL_MAX)) count++;
    }
    return count;
}

cJSON *launcher__json_find_menu(cJSON *root, const char *menu_label) {
    if (root == NULL) return NULL;
    if (menu_label == NULL || menu_label[0] == '\0' || strcmp(menu_label, BRUCE_LAUNCHER_ROOT_MENU_LABEL) == 0) {
        return root;
    }
    cJSON *child;
    cJSON_ArrayForEach(child, root) {
        if (!cJSON_IsObject(child) || child->string == NULL) continue;
        char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
        if (launcher__label_from_key(child->string, label, sizeof(label)) && strcmp(label, menu_label) == 0) {
            return child;
        }
    }
    return NULL;
}

cJSON *launcher__json_find_menu_at_path(cJSON *root, const char *const *path, size_t path_depth) {
    cJSON *menu = root;
    for (size_t i = 0; i < path_depth && menu != NULL; ++i) {
        menu = launcher__json_find_menu(menu, path[i]);
    }
    return menu;
}

cJSON *launcher__load_root(void) {
    char *text = NULL;
    size_t size = 0;
    if (!storage__read_file(LAUNCHER__CONFIG_PATH, &text, &size) || size == 0) return NULL;
    cJSON *root = cJSON_ParseWithLength(text, size);
    storage__free(text);
    if (root != NULL && !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = NULL;
    }
    return root;
}

size_t launcher__list_menus(char out_labels[][BRUCE_LAUNCHER_ENTRY_LABEL_MAX], size_t capacity) {
    cJSON *root = launcher__load_root();
    size_t count = launcher__json_menu_labels(root, out_labels, capacity);
    cJSON_Delete(root);
    return count;
}

bruce_result_t launcher__save_root(const cJSON *root) {
    char *serialized = cJSON_Print(root);
    if (serialized == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = BRUCE_OK;
    size_t length = strlen(serialized);
    if (length >= LAUNCHER__JSON_MAX_BYTES) result = BRUCE_ERR_RESOURCE_LIMIT;
    else if (!storage__write_file_atomic(LAUNCHER__CONFIG_PATH, serialized, length)) result = BRUCE_ERR_IO;
    cJSON_free(serialized);
    return result;
}

bool launcher__menu_has_command(const char *menu_label, const char *command) {
    if (command == NULL || command[0] == '\0') return false;
    cJSON *root = launcher__load_root();
    cJSON *menu = launcher__json_find_menu(root, menu_label);
    bool found = launcher__json_has_command(menu, command);
    cJSON_Delete(root);
    return found;
}

/* Prompts for one of `menu_labels[0..menu_count)` with dialog__choice(),
 * appending a synthetic "Cancel" row after them. Returns the chosen label's
 * index, or menu_count on cancel/a dialog failure/an allocation failure --
 * callers treat all three the same way (abort without touching the config).
 *
 * `choices` is heap-allocated rather than a BRUCE_LAUNCHER_MENU_LIST_MAX + 1
 * local array: launcher__add_menu_entry() can run on whatever task called
 * it (e.g. apps_app_main()'s long-press handler, on the "apps" task's small
 * default stack -- see PROCESS__DEFAULT_STACK_BYTES in core/process/process.c),
 * so this function's own frame has to stay small rather than assume a
 * generous caller stack. */
static size_t launcher__prompt_menu(
    const char *label, char menu_labels[][BRUCE_LAUNCHER_ENTRY_LABEL_MAX], size_t menu_count
) {
    bruce_dialog_choice_t *choices = memory__calloc(menu_count + 1, sizeof(*choices));
    if (choices == NULL) return menu_count;
    for (size_t i = 0; i < menu_count; ++i) {
        choices[i] = (bruce_dialog_choice_t){.label = menu_labels[i], .value = menu_labels[i]};
    }
    choices[menu_count] = (bruce_dialog_choice_t){.label = "Cancel", .value = "cancel"};

    char message[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 16];
    snprintf(message, sizeof(message), "Add \"%s\" to:", label);
    size_t selected = 0;
    bruce_result_t result = dialog__choice("Launcher", message, choices, menu_count + 1, &selected);
    size_t chosen =
        result == BRUCE_OK && strcmp(choices[selected].value, "cancel") != 0 ? selected : menu_count;
    memory__free(choices);
    return chosen;
}

/**
 * @brief Appends a command entry to the launcher menu tree, asking where.
 *
 * See core_sdk/launcher.h for the full contract. Owns the whole interaction:
 * when the config has more than one destination (the root plus its own
 * submenus - see launcher__list_menus()), prompts with dialog__choice() and
 * proceeds with whatever was picked; with only the root to choose from, adds
 * there directly. Also reports the outcome via dialog__message() - "Added to
 * X"/"Already in X" - so callers don't have to. A cancelled prompt returns
 * BRUCE_ERR_CANCELLED without showing anything further or touching the
 * file, the same way every other dialog__* cancel stays silent.
 */
bruce_result_t launcher__add_menu_entry(const char *label, const char *icon_name, const char *command) {
    if (label == NULL || label[0] == '\0' || command == NULL || command[0] == '\0') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    cJSON *root = launcher__load_root();
    if (root == NULL) return BRUCE_ERR_NOT_FOUND;

    /* Heap-allocated, not a BRUCE_LAUNCHER_MENU_LIST_MAX local array (that's
     * 33 * BRUCE_LAUNCHER_ENTRY_LABEL_MAX = 1584 bytes) -- see
     * launcher__prompt_menu()'s comment on why this function's frame has to
     * stay small regardless of the caller's own stack budget. */
    char(*menu_labels)[BRUCE_LAUNCHER_ENTRY_LABEL_MAX] =
        memory__calloc(BRUCE_LAUNCHER_MENU_LIST_MAX, sizeof(*menu_labels));
    if (menu_labels == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NO_MEMORY;
    }
    size_t menu_count = launcher__json_menu_labels(root, menu_labels, BRUCE_LAUNCHER_MENU_LIST_MAX);

    size_t chosen = 0;
    if (menu_count > 1) {
        chosen = launcher__prompt_menu(label, menu_labels, menu_count);
        if (chosen >= menu_count) {
            memory__free(menu_labels);
            cJSON_Delete(root);
            return BRUCE_ERR_CANCELLED;
        }
    }
    char menu_label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
    snprintf(menu_label, sizeof(menu_label), "%s", menu_labels[chosen]);
    memory__free(menu_labels);

    cJSON *menu = launcher__json_find_menu(root, menu_label);
    if (menu == NULL) {
        cJSON_Delete(root);
        return BRUCE_ERR_NOT_FOUND;
    }

    char message[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 16];
    if (launcher__json_has_command(menu, command)) {
        cJSON_Delete(root);
        snprintf(message, sizeof(message), "Already in %s", menu_label);
        (void)dialog__message(BRUCE_DIALOG_INFO, "Launcher", message);
        return BRUCE_OK;
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

    if (result == BRUCE_OK) {
        snprintf(message, sizeof(message), "Added to %s", menu_label);
        (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Launcher", message);
    }
    return result;
}
