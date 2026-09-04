#include "core_sdk/launcher.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "core/storage/storage.h"
#include "core_sdk/dialog.h"
#include "launcher_internal.h"

/* Mirrors modules/bruce_launcher/bruce_launcher_menu.c's own
 * BRUCE_LAUNCHER_CONFIG_PATH/BRUCE_LAUNCHER_JSON_MAX/BRUCE_LAUNCHER_MAX_ENTRIES
 * - keep all three in sync if any of them change there. This file only ever
 * appends one entry to the same config that module owns loading/parsing
 * for, so it must never grow the file past what that loader will actually
 * read back (a file at or over the size limit is treated as unreadable and
 * silently replaced with the embedded default on the next load, and a menu
 * over the entry limit just has its extra entries dropped at load time). */
#define LAUNCHER__CONFIG_PATH "/config/launcher.conf"
#define LAUNCHER__JSON_MAX_BYTES 8192
#define LAUNCHER__MAX_ENTRIES 32

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

/* Loads /config/launcher.conf into a parsed object, or NULL if it doesn't
 * exist yet, is empty, or isn't a JSON object - the same shape
 * bruce_launcher__menu_load() requires before it'll use a file instead of
 * falling back to the embedded default. Caller owns the result via
 * cJSON_Delete(). */
static cJSON *launcher__load_root(void) {
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
 * index, or menu_count on cancel/a dialog failure -- callers treat that the
 * same way (abort without touching the config). */
static size_t launcher__prompt_menu(
    const char *label, char menu_labels[][BRUCE_LAUNCHER_ENTRY_LABEL_MAX], size_t menu_count
) {
    bruce_dialog_choice_t choices[BRUCE_LAUNCHER_MENU_LIST_MAX + 1];
    for (size_t i = 0; i < menu_count; ++i) {
        choices[i] = (bruce_dialog_choice_t){.label = menu_labels[i], .value = menu_labels[i]};
    }
    choices[menu_count] = (bruce_dialog_choice_t){.label = "Cancel", .value = "cancel"};

    char message[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 16];
    snprintf(message, sizeof(message), "Add \"%s\" to:", label);
    size_t selected = 0;
    bruce_result_t result = dialog__choice("Launcher", message, choices, menu_count + 1, &selected);
    return result == BRUCE_OK && strcmp(choices[selected].value, "cancel") != 0 ? selected : menu_count;
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

    char menu_labels[BRUCE_LAUNCHER_MENU_LIST_MAX][BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
    size_t menu_count = launcher__json_menu_labels(root, menu_labels, BRUCE_LAUNCHER_MENU_LIST_MAX);

    size_t chosen = 0;
    if (menu_count > 1) {
        chosen = launcher__prompt_menu(label, menu_labels, menu_count);
        if (chosen >= menu_count) {
            cJSON_Delete(root);
            return BRUCE_ERR_CANCELLED;
        }
    }
    const char *menu_label = menu_labels[chosen];

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

    char *serialized = cJSON_Print(root);
    cJSON_Delete(root);
    if (serialized == NULL) return BRUCE_ERR_NO_MEMORY;

    bruce_result_t result = BRUCE_OK;
    size_t length = strlen(serialized);
    if (length >= LAUNCHER__JSON_MAX_BYTES) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
    } else if (!storage__write_file_atomic(LAUNCHER__CONFIG_PATH, serialized, length)) {
        result = BRUCE_ERR_IO;
    }
    cJSON_free(serialized);

    if (result == BRUCE_OK) {
        snprintf(message, sizeof(message), "Added to %s", menu_label);
        (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Launcher", message);
    }
    return result;
}
