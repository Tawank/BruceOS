#include "apps_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"

#define APPS_LABEL_MAX (BRUCE_MANIFEST_APP_NAME_MAX + 16u)

typedef struct {
    char name[APPS_LABEL_MAX];
    char label[APPS_LABEL_MAX + 7];
    char path[BRUCE_STORAGE_PATH_MAX];
    const char *type;
} apps_entry_t;

static bool apps__has_extension(const char *name, const char *extension) {
    size_t name_length = strlen(name);
    size_t extension_length = strlen(extension);
    return name_length > extension_length &&
           strcasecmp(name + name_length - extension_length, extension) == 0;
}

static bool apps__resume_after_handoff(void) {
    bruce_process_snapshot_t snapshot;
    bruce_process_id_t self = process__current_id();
    if (self == BRUCE_PROCESS_ID_INVALID || process__snapshot(self, &snapshot) != BRUCE_OK ||
        snapshot.state != BRUCE_PROCESS_BACKGROUND) {
        return false;
    }
    do {
        if (runtime__delay(20) != BRUCE_OK || process__snapshot(self, &snapshot) != BRUCE_OK) return false;
    } while (snapshot.state == BRUCE_PROCESS_BACKGROUND);
    return snapshot.state == BRUCE_PROCESS_FOREGROUND;
}

static size_t apps__directory_count(const char *path) {
    size_t count = 0;
    return storage__list(path, NULL, 0, &count) == BRUCE_OK ? count : 0;
}

static void apps__set_label(apps_entry_t *app, const char *filename, const char *extension) {
    bruce_app_inspection_t *inspection;
    if (strcasecmp(extension, ".js") == 0) {
        inspection = manifest__inspect_javascript(app->path);
    } else if (strcasecmp(extension, ".wasm") == 0) {
        inspection = manifest__inspect_wasm(app->path);
    } else {
        inspection = manifest__inspect_elf(app->path);
    }
    const char *name = inspection != NULL && inspection->manifest.app_name[0] != '\0'
                           ? inspection->manifest.app_name
                           : filename;
    snprintf(app->name, sizeof(app->name), "%s", name);
    snprintf(app->label, sizeof(app->label), "%s", app->name);
    app->type =
        strcasecmp(extension, ".wasm") == 0 ? "wasm" : (strcasecmp(extension, ".js") == 0 ? "js" : "elf");
    memory__free(inspection);
}

static void apps__disambiguate_labels(apps_entry_t *apps, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < count; ++j) {
            if (i != j && strcasecmp(apps[i].name, apps[j].name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            char name[sizeof(apps[i].label)];
            const char *type = apps[i].type;
            size_t suffix_length = strlen(type) + 3u;
            size_t name_length;
            snprintf(name, sizeof(name), "%s", apps[i].label);
            name_length = strlen(name);
            if (name_length + suffix_length >= sizeof(apps[i].label)) {
                name_length = sizeof(apps[i].label) - suffix_length - 1u;
            }
            memcpy(apps[i].label, name, name_length);
            apps[i].label[name_length++] = ' ';
            apps[i].label[name_length++] = '(';
            memcpy(apps[i].label + name_length, type, strlen(type));
            name_length += strlen(type);
            apps[i].label[name_length++] = ')';
            apps[i].label[name_length] = '\0';
        }
    }
}

static bruce_result_t
apps__scan_directory(const char *directory, apps_entry_t *apps, size_t capacity, size_t *in_out_count) {
    size_t entry_count = apps__directory_count(directory);
    if (entry_count == 0) return BRUCE_OK;

    bruce_storage_entry_t *entries = memory__calloc(entry_count, sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = storage__list(directory, entries, entry_count, &entry_count);
    for (size_t i = 0; result == BRUCE_OK && i < entry_count && *in_out_count < capacity; ++i) {
        if (entries[i].type != BRUCE_STORAGE_ENTRY_FILE) continue;
        const char *extension = apps__has_extension(entries[i].name, ".js")
                                    ? ".js"
                                    : (apps__has_extension(entries[i].name, ".wasm") ? ".wasm" : ".elf");
        if (!apps__has_extension(entries[i].name, extension)) continue;

        apps_entry_t *app = &apps[(*in_out_count)++];
        int length = snprintf(app->path, sizeof(app->path), "%s/%s", directory, entries[i].name);
        if (length < 0 || (size_t)length >= sizeof(app->path)) {
            --(*in_out_count);
            continue;
        }
        apps__set_label(app, entries[i].name, extension);
    }
    memory__free(entries);
    return result;
}

static int apps__compare(const void *left, const void *right) {
    const apps_entry_t *a = left;
    const apps_entry_t *b = right;
    int label_order = strcasecmp(a->label, b->label);
    return label_order != 0 ? label_order : strcasecmp(a->path, b->path);
}

static void apps__show_error(const char *action, bruce_result_t result) {
    char message[160];
    ext_mem_loader__format_error_message(action, result, message, sizeof(message));
    (void)dialog__message(BRUCE_DIALOG_ERROR, "Apps", message);
}

int apps_app_main(int argc, char **argv) {
    bool gui = runtime__gui_requested();

    const char *directories[] = {"/apps", "/scripts"};
    size_t capacity = 0;
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        size_t count = apps__directory_count(directories[i]);
        if (count > SIZE_MAX - capacity) return BRUCE_ERR_RESOURCE_LIMIT;
        capacity += count;
    }
    if (capacity == 0) {
        (void)dialog__message(BRUCE_DIALOG_INFO, "Apps", "No .elf, .wasm, or .js files in /apps or /scripts");
        return BRUCE_OK;
    }

    apps_entry_t *apps = memory__calloc(capacity, sizeof(*apps));
    if (apps == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t count = 0;
    bruce_result_t result = BRUCE_OK;
    for (size_t i = 0; result == BRUCE_OK && i < sizeof(directories) / sizeof(directories[0]); ++i) {
        result = apps__scan_directory(directories[i], apps, capacity, &count);
    }
    if (result != BRUCE_OK) {
        memory__free(apps);
        return result;
    }
    if (count == 0) {
        memory__free(apps);
        (void)dialog__message(BRUCE_DIALOG_INFO, "Apps", "No .elf, .wasm, or .js files in /apps or /scripts");
        return BRUCE_OK;
    }

    apps__disambiguate_labels(apps, count);
    qsort(apps, count, sizeof(*apps), apps__compare);
    bruce_dialog_choice_t *choices = memory__calloc(count, sizeof(*choices));
    if (choices == NULL) {
        memory__free(apps);
        return BRUCE_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < count; ++i) {
        choices[i].label = apps[i].label;
        choices[i].value = apps[i].path;
        choices[i].icon_name = NULL;
        choices[i].right_text = NULL;
    }

    for (;;) {
        size_t selected = 0;
        result = dialog__choice_launcher("Apps", NULL, choices, count, &selected);
        if (result == BRUCE_ERR_CANCELLED && apps__resume_after_handoff()) continue;
        if (result == BRUCE_ERR_CANCELLED) break;
        if (result != BRUCE_OK) {
            apps__show_error("Browse", result);
            break;
        }

        const bruce_environment_variable_t gui_env[] = {
            {.name = "GUI", .value = "1"}
        };
        int process = app_runner__run_path_with_environment(
            apps[selected].path, NULL, BRUCE_LAUNCH_FOREGROUND, gui ? gui_env : NULL, gui ? 1u : 0u
        );
        if (process <= 0) apps__show_error("Launch", (bruce_result_t)process);
    }

    memory__free(choices);
    memory__free(apps);
    return result == BRUCE_ERR_CANCELLED ? BRUCE_OK : result;
}
