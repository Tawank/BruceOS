#include "app_settings_app.h"

#include "args.h"
#include "cJSON.h"

#include "core_sdk/dialog.h"
#include "core_sdk/format.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

/* Core-private: reads/writes /config/permissions.json directly. Every other
 * built-in app is restricted to core_sdk headers (enforced by the
 * bruce_sdk_builtin_*_check targets in src/CMakeLists.txt) because
 * storage__open()/storage__list() (core_sdk/storage.h) unconditionally refuse
 * that path - see storage__is_protected_path() in core/storage/storage.c.
 * That protection is exactly what this app needs to see through in order to
 * enumerate every app with a saved decision (permission__get_saved()/
 * permission__set() in core_sdk/permission.h only work one file+permission at
 * a time, and there is deliberately no enumeration API at that layer). Per-
 * value reads/writes below still go through permission__get_saved()/
 * permission__set() rather than this file, so they stay consistent with
 * core/permission/permission.c's in-memory cache; only enumeration and
 * "forget" bypass it. This module is excluded from the core_sdk-only-headers
 * build check the same way modules/selftest and modules/device_bus are. */
#include "core/appdata/appdata.h"
#include "core/storage/storage.h"

#include <stdio.h>
#include <string.h>

#define PERMISSIONS__FILE_PATH "/config/permissions.json"
#define PERMISSIONS__MAX_APPS 32

typedef struct {
    char name[BRUCE_PERMISSION_FILE_NAME_MAX];
} permissions__app_t;

typedef struct {
    bruce_permission_t permission;
} permissions__entry_t;

/* Parses /config/permissions.json, returning an empty object if the file is
 * missing or unreadable so callers can treat "nothing saved yet" uniformly.
 * Caller owns the returned cJSON and must cJSON_Delete() it. */
static cJSON *permissions__load_raw(void) {
    char *text = NULL;
    size_t size = 0;
    if (!storage__read_file(PERMISSIONS__FILE_PATH, &text, &size) || size == 0) {
        if (text != NULL) storage__free(text);
        return cJSON_CreateObject();
    }
    cJSON *root = cJSON_ParseWithLength(text, size);
    storage__free(text);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) cJSON_Delete(root);
        return cJSON_CreateObject();
    }
    return root;
}

static bool permissions__save_raw(cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    if (text == NULL) return false;
    bool saved = storage__mkdir_internal("/config") &&
                 storage__write_file_atomic(PERMISSIONS__FILE_PATH, text, strlen(text));
    cJSON_free(text);
    return saved;
}

static size_t permissions__list_apps(permissions__app_t *out, size_t capacity) {
    cJSON *root = permissions__load_raw();
    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (item->string == NULL || !cJSON_IsObject(item) || count >= capacity) continue;
        snprintf(out[count].name, sizeof(out[count].name), "%s", item->string);
        count++;
    }
    cJSON_Delete(root);
    return count;
}

static size_t permissions__list_permissions(const char *app, permissions__entry_t *out, size_t capacity) {
    cJSON *root = permissions__load_raw();
    cJSON *app_obj = cJSON_GetObjectItemCaseSensitive(root, app);
    size_t count = 0;
    if (app_obj != NULL && cJSON_IsObject(app_obj)) {
        cJSON *perm_item;
        cJSON_ArrayForEach(perm_item, app_obj) {
            bruce_permission_t permission;
            if (perm_item->string == NULL || count >= capacity ||
                !permission__from_name(perm_item->string, &permission)) {
                continue;
            }
            out[count++].permission = permission;
        }
    }
    cJSON_Delete(root);
    return count;
}

/* Removes one permission for `app`, or every permission for `app` if
 * `permission` is NULL. This edits the file directly rather than going
 * through permission__set(), so - unlike every other write in this module -
 * it is invisible to core/permission/permission.c's in-memory cache for the
 * rest of the current boot: any app whose permissions were already consulted
 * this boot keeps answering from that cache (and would clobber this edit back
 * on its next unrelated permission__set() elsewhere) until reboot. Callers
 * are told this via the CLI/GUI copy below. */
static bool permissions__forget_in(cJSON *app_obj, const bruce_permission_t *permission) {
    bool changed = false;
    for (int p = 0; p < BRUCE_PERMISSION_COUNT; ++p) {
        if (permission != NULL && p != (int)*permission) continue;
        const char *name = permission__name((bruce_permission_t)p);
        if (cJSON_GetObjectItemCaseSensitive(app_obj, name) == NULL) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(app_obj, name);
        changed = true;
    }
    return changed;
}

/* Keeps the app's appdata path, so its data is still found afterwards. */
static bool permissions__forget(const char *app, const bruce_permission_t *permission) {
    cJSON *root = permissions__load_raw();
    cJSON *app_obj = cJSON_GetObjectItemCaseSensitive(root, app);
    bool changed = cJSON_IsObject(app_obj) && permissions__forget_in(app_obj, permission);
    if (changed && app_obj->child == NULL) cJSON_DeleteItemFromObjectCaseSensitive(root, app);
    bool saved = !changed || permissions__save_raw(root);
    cJSON_Delete(root);
    return changed && saved;
}

static bool permissions__forget_all(void) {
    cJSON *root = permissions__load_raw();
    cJSON *app_obj = root->child;
    while (app_obj != NULL) {
        cJSON *next = app_obj->next;
        if (cJSON_IsObject(app_obj)) permissions__forget_in(app_obj, NULL);
        if (!cJSON_IsObject(app_obj) || app_obj->child == NULL)
            cJSON_Delete(cJSON_DetachItemViaPointer(root, app_obj));
        app_obj = next;
    }
    bool saved = permissions__save_raw(root);
    cJSON_Delete(root);
    return saved;
}

static const char *app_settings__location(bool on_sd) { return on_sd ? "SD card" : "Internal"; }

static void app_settings__usage_text(const char *app, char *out, size_t capacity) {
    size_t bytes = 0;
    bruce_result_t result = appdata__app_usage(app, &bytes);
    if (result == BRUCE_ERR_INVALID_STATE) {
        snprintf(out, capacity, "SD card missing");
    } else if (result != BRUCE_OK) {
        snprintf(out, capacity, "?");
    } else {
        format__bytes_human(bytes, out, capacity);
    }
}

static const char *app_settings__error_text(bruce_result_t result) {
    switch (result) {
        case BRUCE_ERR_BUSY: return "Close the app first.";
        case BRUCE_ERR_INVALID_STATE: return "No SD card inserted.";
        case BRUCE_ERR_NO_MEMORY: return "Not enough memory.";
        default: return "Storage error.";
    }
}

static bool permissions__parse_state(const char *text, bool *out) {
    if (text == NULL) return false;
    if (strcmp(text, "allow") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(text, "deny") == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* CLI                                                                        */
/* -------------------------------------------------------------------------- */

static int app_settings__list_cli(void) {
    permissions__app_t apps[PERMISSIONS__MAX_APPS];
    size_t app_count = permissions__list_apps(apps, PERMISSIONS__MAX_APPS);
    if (app_count == 0) {
        stdio__printf("No app has requested a permission yet\n");
        return BRUCE_OK;
    }
    for (size_t i = 0; i < app_count; ++i) {
        stdio__printf("%s\n", apps[i].name);
        permissions__entry_t entries[BRUCE_PERMISSION_COUNT];
        size_t entry_count = permissions__list_permissions(apps[i].name, entries, BRUCE_PERMISSION_COUNT);
        for (size_t j = 0; j < entry_count; ++j) {
            bool allowed = false;
            bruce_result_t saved = permission__get_saved(apps[i].name, entries[j].permission, &allowed);
            stdio__printf(
                "  %-10s %s\n",
                permission__name(entries[j].permission),
                saved == BRUCE_OK ? (allowed ? "allow" : "deny") : "unknown"
            );
        }
    }
    return BRUCE_OK;
}

static int app_settings__get_cli(const char *app, const char *permission_name) {
    bruce_permission_t permission;
    if (app == NULL || permission_name == NULL || !permission__from_name(permission_name, &permission)) {
        stdio__printf("Unknown permission: %s\n", permission_name != NULL ? permission_name : "(none)");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bool allowed = false;
    bruce_result_t result = permission__get_saved(app, permission, &allowed);
    if (result != BRUCE_OK) {
        stdio__printf("%s: %s not decided yet\n", app, permission_name);
        return BRUCE_OK;
    }
    stdio__printf("%s: %s %s\n", app, permission_name, allowed ? "allow" : "deny");
    return BRUCE_OK;
}

static int app_settings__set_cli(const char *app, const char *permission_name, const char *state) {
    bruce_permission_t permission;
    bool allowed = false;
    if (app == NULL || permission_name == NULL || !permission__from_name(permission_name, &permission) ||
        !permissions__parse_state(state, &allowed)) {
        stdio__printf("Usage: appsettings set <app> <permission> <allow|deny>\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = permission__set(app, permission, allowed);
    if (result != BRUCE_OK) {
        stdio__printf("%s: failed to set %s\n", app, permission_name);
        return result;
    }
    stdio__printf("%s: %s set to %s\n", app, permission_name, allowed ? "allow" : "deny");
    return BRUCE_OK;
}

static int app_settings__forget_cli(const char *app, const char *permission_name) {
    if (app == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_permission_t permission;
    bool has_permission = permission_name != NULL;
    if (has_permission && !permission__from_name(permission_name, &permission)) {
        stdio__printf("Unknown permission: %s\n", permission_name);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bool forgot = permissions__forget(app, has_permission ? &permission : NULL);
    if (!forgot) {
        stdio__printf("%s: nothing to forget\n", app);
        return BRUCE_OK;
    }
    stdio__printf("%s: forgotten. Takes effect after reboot.\n", app);
    return BRUCE_OK;
}

static int app_settings__wipe_cli(const char *confirm) {
    if (confirm == NULL || strcmp(confirm, "confirm") != 0) {
        stdio__printf("Refusing to wipe every saved decision without 'confirm'\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bool removed = permissions__forget_all();
    stdio__printf(
        removed ? "All saved permission decisions cleared. Apps already checked this boot keep answering "
                  "from cache until reboot.\n"
                : "Failed to clear the permissions file\n"
    );
    return removed ? BRUCE_OK : BRUCE_ERR_IO;
}

static int app_settings__data_cli(const char *only_app) {
    permissions__app_t apps[PERMISSIONS__MAX_APPS];
    size_t app_count = only_app != NULL ? 1 : permissions__list_apps(apps, PERMISSIONS__MAX_APPS);
    if (only_app != NULL) snprintf(apps[0].name, sizeof(apps[0].name), "%s", only_app);
    stdio__printf("Default for new apps: %s\n", app_settings__location(appdata__default_on_sd()));
    for (size_t i = 0; i < app_count; ++i) {
        char path[BRUCE_STORAGE_PATH_MAX] = "?";
        char usage[24];
        appdata__app_path(apps[i].name, path, sizeof(path));
        app_settings__usage_text(apps[i].name, usage, sizeof(usage));
        stdio__printf("%-24s %-10s %s\n", apps[i].name, usage, path);
    }
    return BRUCE_OK;
}

static bool app_settings__parse_location(const char *text, bool *out_on_sd) {
    if (text == NULL) return false;
    if (strcmp(text, "sd") == 0 || strcmp(text, "sdcard") == 0) {
        *out_on_sd = true;
        return true;
    }
    if (strcmp(text, "internal") == 0) {
        *out_on_sd = false;
        return true;
    }
    return false;
}

static int app_settings__move_cli(const char *app, const char *location) {
    bool on_sd = false;
    if (app == NULL || !app_settings__parse_location(location, &on_sd)) {
        stdio__printf("Usage: appsettings move <app> <internal|sd>\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = appdata__move(app, on_sd);
    stdio__printf(
        "%s: %s\n", app, result == BRUCE_OK ? app_settings__location(on_sd) : app_settings__error_text(result)
    );
    return result;
}

static int app_settings__clear_cli(const char *app, const char *confirm) {
    if (app == NULL || confirm == NULL || strcmp(confirm, "confirm") != 0) {
        stdio__printf("Usage: appsettings clear <app> confirm\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = appdata__clear(app);
    stdio__printf("%s: %s\n", app, result == BRUCE_OK ? "data cleared" : app_settings__error_text(result));
    return result;
}

static int app_settings__default_cli(const char *location) {
    bool on_sd = false;
    if (location == NULL) {
        stdio__printf("%s\n", app_settings__location(appdata__default_on_sd()));
        return BRUCE_OK;
    }
    if (!app_settings__parse_location(location, &on_sd)) {
        stdio__printf("Usage: appsettings default [internal|sd]\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = appdata__set_default_on_sd(on_sd);
    stdio__printf("New apps store data on: %s\n", app_settings__location(on_sd));
    return result;
}

/* -------------------------------------------------------------------------- */
/* GUI                                                                        */
/* -------------------------------------------------------------------------- */

#define APP_SETTINGS__TITLE "App Settings"

static void app_settings__set_choice(
    bruce_dialog_choice_t *choice, const char *label, const char *value, const char *right
) {
    choice->label = label;
    choice->value = value;
    choice->icon_name = NULL;
    choice->right_text = right;
}

static bool app_settings__confirm(const char *title, const char *message, const char *action) {
    bruce_dialog_choice_t choices[2];
    app_settings__set_choice(&choices[0], "Cancel", "cancel", NULL);
    app_settings__set_choice(&choices[1], action, "ok", NULL);
    size_t selected = 0;
    return dialog__choice_launcher(title, message, choices, 2, &selected) == BRUCE_OK && selected == 1;
}

static void app_settings__show_result(bruce_result_t result, const char *done) {
    (void)dialog__message(
        result == BRUCE_OK ? BRUCE_DIALOG_INFO : BRUCE_DIALOG_ERROR,
        APP_SETTINGS__TITLE,
        result == BRUCE_OK ? done : app_settings__error_text(result)
    );
}

static void app_settings__gui_app(const char *app) {
    for (;;) {
        permissions__entry_t entries[BRUCE_PERMISSION_COUNT];
        size_t entry_count = permissions__list_permissions(app, entries, BRUCE_PERMISSION_COUNT);
        bool on_sd = appdata__app_on_sd(app);

        char labels[BRUCE_PERMISSION_COUNT][24];
        char storage_label[32];
        char usage[24];
        bruce_dialog_choice_t choices[BRUCE_PERMISSION_COUNT + 4];
        size_t count = 0;
        for (size_t i = 0; i < entry_count; ++i) {
            bool allowed = false;
            bruce_result_t saved = permission__get_saved(app, entries[i].permission, &allowed);
            snprintf(labels[i], sizeof(labels[i]), "%s", permission__name(entries[i].permission));
            app_settings__set_choice(
                &choices[count++],
                labels[i],
                permission__name(entries[i].permission),
                saved == BRUCE_OK ? (allowed ? "Allow" : "Deny") : "Unknown"
            );
        }
        snprintf(storage_label, sizeof(storage_label), "Data on %s", app_settings__location(on_sd));
        app_settings__usage_text(app, usage, sizeof(usage));
        app_settings__set_choice(&choices[count++], storage_label, "move", usage);
        app_settings__set_choice(&choices[count++], "Clear data", "clear", NULL);
        if (entry_count > 0)
            app_settings__set_choice(&choices[count++], "Forget permissions", "forget", NULL);
        app_settings__set_choice(&choices[count++], "Back", "back", NULL);

        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher(app, NULL, choices, count, &selected);
        if (result != BRUCE_OK || strcmp(choices[selected].value, "back") == 0) return;
        const char *value = choices[selected].value;

        if (strcmp(value, "move") == 0) {
            const char *target = app_settings__location(!on_sd);
            char message[64];
            snprintf(message, sizeof(message), "Move %s data to %s?", app, target);
            if (app_settings__confirm("Move data", message, "Move")) {
                app_settings__show_result(
                    appdata__move(app, !on_sd), on_sd ? "Moved to internal storage." : "Moved to the SD card."
                );
            }
        } else if (strcmp(value, "clear") == 0) {
            if (app_settings__confirm("Clear data", "Delete every file this app saved?", "Clear")) {
                app_settings__show_result(appdata__clear(app), "Data cleared.");
            }
        } else if (strcmp(value, "forget") == 0) {
            bool forgot = permissions__forget(app, NULL);
            (void)dialog__message(
                forgot ? BRUCE_DIALOG_INFO : BRUCE_DIALOG_ERROR,
                APP_SETTINGS__TITLE,
                forgot ? "Forgotten. Takes effect after reboot." : "Nothing to forget."
            );
        } else {
            bool allowed = false;
            bruce_result_t saved = permission__get_saved(app, entries[selected].permission, &allowed);
            (void)permission__set(app, entries[selected].permission, saved != BRUCE_OK || !allowed);
        }
    }
}

static int app_settings__gui(void) {
    for (;;) {
        permissions__app_t apps[PERMISSIONS__MAX_APPS];
        size_t app_count = permissions__list_apps(apps, PERMISSIONS__MAX_APPS);
        bool default_sd = appdata__default_on_sd();

        bruce_dialog_choice_t choices[PERMISSIONS__MAX_APPS + 3];
        size_t count = 0;
        for (size_t i = 0; i < app_count; ++i) {
            app_settings__set_choice(
                &choices[count++], apps[i].name, apps[i].name, appdata__app_on_sd(apps[i].name) ? "SD" : NULL
            );
        }
        app_settings__set_choice(
            &choices[count++], "New apps store data", "default", app_settings__location(default_sd)
        );
        app_settings__set_choice(&choices[count++], "Wipe all permissions", "wipe", NULL);
        app_settings__set_choice(&choices[count++], "Back", "back", NULL);

        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher(APP_SETTINGS__TITLE, NULL, choices, count, &selected);
        if (result == BRUCE_ERR_CANCELLED ||
            (result == BRUCE_OK && strcmp(choices[selected].value, "back") == 0)) {
            return BRUCE_OK;
        }
        if (result != BRUCE_OK) return result;
        const char *value = choices[selected].value;

        if (strcmp(value, "default") == 0) {
            (void)appdata__set_default_on_sd(!default_sd);
        } else if (strcmp(value, "wipe") == 0) {
            if (app_settings__confirm(
                    "Wipe all?", "Clears every app's saved permission decisions", "Wipe all"
                )) {
                (void)app_settings__wipe_cli("confirm");
            }
        } else {
            app_settings__gui_app(apps[selected].name);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                               */
/* -------------------------------------------------------------------------- */

int app_settings_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Manage app permissions and app data.");

    ArgParser *list = ap_new_cmd(root, "list");
    ArgParser *get = ap_new_cmd(root, "get");
    ArgParser *set = ap_new_cmd(root, "set");
    ArgParser *forget = ap_new_cmd(root, "forget");
    ArgParser *wipe = ap_new_cmd(root, "wipe");
    ArgParser *data = ap_new_cmd(root, "data");
    ArgParser *move = ap_new_cmd(root, "move");
    ArgParser *clear = ap_new_cmd(root, "clear");
    ArgParser *default_cmd = ap_new_cmd(root, "default");
    ArgParser *parsers[] = {list, get, set, forget, wipe, data, move, clear, default_cmd};
    for (size_t i = 0; i < sizeof(parsers) / sizeof(parsers[0]); ++i) {
        if (parsers[i] == NULL) {
            ap_free(root);
            return BRUCE_ERR_NO_MEMORY;
        }
    }

    ap_set_helptext(list, "List every app with a saved permission decision.");
    ap_set_helptext(get, "Show the saved decision for one app/permission pair.");
    ap_add_required_arg(get, "app", "App file name, e.g. game.elf");
    ap_add_required_arg(get, "permission", "Permission name, e.g. storage");
    ap_set_helptext(set, "Allow or deny a permission for an app.");
    ap_add_required_arg(set, "app", "App file name, e.g. game.elf");
    ap_add_required_arg(set, "permission", "Permission name, e.g. storage");
    ap_add_required_arg(set, "state", "allow or deny");
    ap_set_helptext(forget, "Clear a saved decision so the app is re-prompted on its next launch.");
    ap_add_required_arg(forget, "app", "App file name, e.g. game.elf");
    ap_add_optional_arg(
        forget, "permission", "Permission name; omit to forget every permission for this app"
    );
    ap_set_helptext(wipe, "Delete every saved permission decision for every app.");
    ap_add_required_arg(wipe, "confirm", "Must be the literal word 'confirm'");
    ap_set_helptext(data, "Show where apps keep their data and how much they use.");
    ap_add_optional_arg(data, "app", "App file name; omit for every app");
    ap_set_helptext(move, "Move an app's data to internal storage or the SD card.");
    ap_add_required_arg(move, "app", "App file name, e.g. game.elf");
    ap_add_required_arg(move, "location", "internal or sd");
    ap_set_helptext(clear, "Delete every file an app saved.");
    ap_add_required_arg(clear, "app", "App file name, e.g. game.elf");
    ap_add_required_arg(clear, "confirm", "Must be the literal word 'confirm'");
    ap_set_helptext(default_cmd, "Show or set where new apps store their data.");
    ap_add_optional_arg(default_cmd, "location", "internal or sd");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    ArgParser *root_action = ap_get_cmd_parser(root);
    bool gui = runtime__is_gui_requested();
    int result;
    if (root_action == NULL) {
        result = gui ? app_settings__gui() : app_settings__list_cli();
    } else if (root_action == list) {
        result = app_settings__list_cli();
    } else if (root_action == get) {
        result = app_settings__get_cli(ap_get_arg(get, "app"), ap_get_arg(get, "permission"));
    } else if (root_action == set) {
        result = app_settings__set_cli(
            ap_get_arg(set, "app"), ap_get_arg(set, "permission"), ap_get_arg(set, "state")
        );
    } else if (root_action == forget) {
        result = app_settings__forget_cli(ap_get_arg(forget, "app"), ap_get_arg(forget, "permission"));
    } else if (root_action == wipe) {
        result = app_settings__wipe_cli(ap_get_arg(wipe, "confirm"));
    } else if (root_action == data) {
        result = app_settings__data_cli(ap_get_arg(data, "app"));
    } else if (root_action == move) {
        result = app_settings__move_cli(ap_get_arg(move, "app"), ap_get_arg(move, "location"));
    } else if (root_action == clear) {
        result = app_settings__clear_cli(ap_get_arg(clear, "app"), ap_get_arg(clear, "confirm"));
    } else if (root_action == default_cmd) {
        result = app_settings__default_cli(ap_get_arg(default_cmd, "location"));
    } else {
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
