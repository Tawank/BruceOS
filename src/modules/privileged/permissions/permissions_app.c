#include "permissions_app.h"

#include "args.h"
#include "cJSON.h"

#include "core_sdk/dialog.h"
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
static bool permissions__forget(const char *app, const bruce_permission_t *permission) {
    cJSON *root = permissions__load_raw();
    cJSON *app_obj = cJSON_GetObjectItemCaseSensitive(root, app);
    bool changed = false;
    if (app_obj != NULL) {
        if (permission == NULL) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, app);
            changed = true;
        } else {
            const char *name = permission__name(*permission);
            if (name != NULL && cJSON_GetObjectItemCaseSensitive(app_obj, name) != NULL) {
                cJSON_DeleteItemFromObjectCaseSensitive(app_obj, name);
                changed = true;
            }
        }
    }
    bool saved = !changed || permissions__save_raw(root);
    cJSON_Delete(root);
    return changed && saved;
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

static int permissions_app__list_cli(void) {
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

static int permissions_app__get_cli(const char *app, const char *permission_name) {
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

static int permissions_app__set_cli(const char *app, const char *permission_name, const char *state) {
    bruce_permission_t permission;
    bool allowed = false;
    if (app == NULL || permission_name == NULL || !permission__from_name(permission_name, &permission) ||
        !permissions__parse_state(state, &allowed)) {
        stdio__printf("Usage: permissions set <app> <permission> <allow|deny>\n");
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

static int permissions_app__forget_cli(const char *app, const char *permission_name) {
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
    stdio__printf(
        "%s: forgotten. Takes effect on this app's next launch, unless its permissions were already "
        "checked this boot - then it takes effect after reboot.\n",
        app
    );
    return BRUCE_OK;
}

static int permissions_app__wipe_cli(const char *confirm) {
    if (confirm == NULL || strcmp(confirm, "confirm") != 0) {
        stdio__printf("Refusing to wipe every saved decision without 'confirm'\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bool removed =
        storage__remove_internal(PERMISSIONS__FILE_PATH) || !storage__exists_internal(PERMISSIONS__FILE_PATH);
    stdio__printf(
        removed ? "All saved permission decisions cleared. Apps already checked this boot keep answering "
                  "from cache until reboot.\n"
                : "Failed to clear the permissions file\n"
    );
    return removed ? BRUCE_OK : BRUCE_ERR_IO;
}

/* -------------------------------------------------------------------------- */
/* GUI                                                                        */
/* -------------------------------------------------------------------------- */

static void permissions_app__gui_app(const char *app) {
    for (;;) {
        permissions__entry_t entries[BRUCE_PERMISSION_COUNT];
        size_t entry_count = permissions__list_permissions(app, entries, BRUCE_PERMISSION_COUNT);
        if (entry_count == 0) return;

        char labels[BRUCE_PERMISSION_COUNT][40];
        bruce_dialog_choice_t choices[BRUCE_PERMISSION_COUNT + 2];
        for (size_t i = 0; i < entry_count; ++i) {
            bool allowed = false;
            bruce_result_t saved = permission__get_saved(app, entries[i].permission, &allowed);
            snprintf(
                labels[i],
                sizeof(labels[i]),
                "%s: %s",
                permission__name(entries[i].permission),
                saved == BRUCE_OK ? (allowed ? "Allow" : "Deny") : "Unknown"
            );
            choices[i].label = labels[i];
            choices[i].value = permission__name(entries[i].permission);
            choices[i].icon_name = NULL;
            choices[i].right_text = saved == BRUCE_OK ? (allowed ? "Allow" : "Deny") : "Unknown";
        }
        choices[entry_count].label = "Forget this app";
        choices[entry_count].value = "forget";
        choices[entry_count].icon_name = NULL;
        choices[entry_count].right_text = NULL;
        choices[entry_count + 1].label = "Back";
        choices[entry_count + 1].value = "back";
        choices[entry_count + 1].icon_name = NULL;
        choices[entry_count + 1].right_text = NULL;

        char title[BRUCE_PERMISSION_FILE_NAME_MAX + 16];
        snprintf(title, sizeof(title), "%s permissions", app);
        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher(title, NULL, choices, entry_count + 2, &selected);
        if (result == BRUCE_ERR_CANCELLED || strcmp(choices[selected].value, "back") == 0) return;
        if (result != BRUCE_OK) return;

        if (strcmp(choices[selected].value, "forget") == 0) {
            (void)permissions__forget(app, NULL);
            return;
        }

        bool allowed = false;
        bruce_result_t saved = permission__get_saved(app, entries[selected].permission, &allowed);
        (void)permission__set(app, entries[selected].permission, saved != BRUCE_OK || !allowed);
    }
}

static int permissions_app__gui(void) {
    for (;;) {
        permissions__app_t apps[PERMISSIONS__MAX_APPS];
        size_t app_count = permissions__list_apps(apps, PERMISSIONS__MAX_APPS);
        if (app_count == 0) {
            (void
            )dialog__message(BRUCE_DIALOG_INFO, "App permissions", "No app has requested a permission yet");
            return BRUCE_OK;
        }

        bruce_dialog_choice_t choices[PERMISSIONS__MAX_APPS + 2];
        for (size_t i = 0; i < app_count; ++i) {
            choices[i].label = apps[i].name;
            choices[i].value = apps[i].name;
            choices[i].icon_name = NULL;
            choices[i].right_text = NULL;
        }
        choices[app_count].label = "Wipe all saved decisions";
        choices[app_count].value = "wipe";
        choices[app_count].icon_name = NULL;
        choices[app_count].right_text = NULL;
        choices[app_count + 1].label = "Back";
        choices[app_count + 1].value = "back";
        choices[app_count + 1].icon_name = NULL;
        choices[app_count + 1].right_text = NULL;

        size_t selected = 0;
        bruce_result_t result =
            dialog__choice_launcher("App permissions", "Select an app", choices, app_count + 2, &selected);
        if (result == BRUCE_ERR_CANCELLED || strcmp(choices[selected].value, "back") == 0) return BRUCE_OK;
        if (result != BRUCE_OK) return result;

        if (strcmp(choices[selected].value, "wipe") == 0) {
            bruce_dialog_choice_t confirm_choices[2] = {
                {.label = "Cancel",   .value = "cancel"},
                {.label = "Wipe all", .value = "wipe"  },
            };
            size_t confirm_selected = 0;
            if (dialog__choice_launcher(
                    "Wipe all?",
                    "Clears every app's saved permission decisions",
                    confirm_choices,
                    2,
                    &confirm_selected
                ) == BRUCE_OK &&
                strcmp(confirm_choices[confirm_selected].value, "wipe") == 0) {
                (void)permissions_app__wipe_cli("confirm");
            }
            continue;
        }

        permissions_app__gui_app(apps[selected].name);
    }
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                               */
/* -------------------------------------------------------------------------- */

int permissions_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "View and manage saved app permission decisions.");

    ArgParser *list = ap_new_cmd(root, "list");
    ArgParser *get = ap_new_cmd(root, "get");
    ArgParser *set = ap_new_cmd(root, "set");
    ArgParser *forget = ap_new_cmd(root, "forget");
    ArgParser *wipe = ap_new_cmd(root, "wipe");
    ArgParser *parsers[] = {list, get, set, forget, wipe};
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

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    ArgParser *root_action = ap_get_cmd_parser(root);
    bool gui = runtime__gui_requested();
    int result;
    if (root_action == NULL) {
        result = gui ? permissions_app__gui() : permissions_app__list_cli();
    } else if (root_action == list) {
        result = permissions_app__list_cli();
    } else if (root_action == get) {
        result = permissions_app__get_cli(ap_get_arg(get, "app"), ap_get_arg(get, "permission"));
    } else if (root_action == set) {
        result = permissions_app__set_cli(
            ap_get_arg(set, "app"), ap_get_arg(set, "permission"), ap_get_arg(set, "state")
        );
    } else if (root_action == forget) {
        result = permissions_app__forget_cli(ap_get_arg(forget, "app"), ap_get_arg(forget, "permission"));
    } else if (root_action == wipe) {
        result = permissions_app__wipe_cli(ap_get_arg(wipe, "confirm"));
    } else {
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
