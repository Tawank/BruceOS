#include "bruce_launcher_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"

#define BRUCE_LAUNCHER_MAX_ENTRIES 32
#define BRUCE_LAUNCHER_LABEL_MAX 80

/* Display layout constants (text size 1: 5x7 font + 1px spacing = 6x8). */
#define BRUCE_LAUNCHER_TITLE_H 10
#define BRUCE_LAUNCHER_ITEM_H 10
#define BRUCE_LAUNCHER_FOOTER_H 10
#define BRUCE_LAUNCHER_TEXT_SIZE 1
#define BRUCE_LAUNCHER_CHAR_W 6
#define BRUCE_LAUNCHER_CHAR_H 8

/* One entry in the launcher menu.  Built-ins are dispatched by name; /apps/
 * entries are dispatched by path. */
typedef struct {
    char label[BRUCE_LAUNCHER_LABEL_MAX];
    char app_name[BRUCE_STORAGE_NAME_MAX];
    char path[BRUCE_STORAGE_PATH_MAX];
    bool is_path;
} bruce_launcher_entry_t;

static bool bruce_launcher__add_builtin(bruce_launcher_entry_t *entries, int *count, int capacity,
                                        const char *app_name, const char *label)
{
    if (*count >= capacity) {
        return false;
    }
    bruce_launcher_entry_t *entry = &entries[(*count)++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    strncpy(entry->app_name, app_name, sizeof(entry->app_name) - 1);
    entry->is_path = false;
    return true;
}

static bool bruce_launcher__add_app(bruce_launcher_entry_t *entries, int *count, int capacity,
                                    const char *path, const char *label)
{
    if (*count >= capacity) {
        return false;
    }
    bruce_launcher_entry_t *entry = &entries[(*count)++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->is_path = true;
    return true;
}

/* Discovers applications in /apps/ by extracting their manifest metadata.
 * Files without a manifest are skipped; the universal manifest__inspect_path()
 * auto-detects the format (ELF section, JS comment block, etc.) so the
 * launcher does not need format-specific knowledge. */
static int bruce_launcher__discover_apps(bruce_launcher_entry_t *entries, int capacity)
{
    bruce_storage_entry_t storage_entries[BRUCE_LAUNCHER_MAX_ENTRIES];
    size_t discovered_count = 0;
    bruce_result_t list_result = storage__list("/apps", storage_entries, sizeof(storage_entries) / sizeof(storage_entries[0]),
                                               &discovered_count);
    if (list_result != BRUCE_OK) {
        return 0;
    }

    int added = 0;
    for (size_t i = 0; i < discovered_count && added < capacity; ++i) {
        if (storage_entries[i].type != BRUCE_STORAGE_ENTRY_FILE) {
            continue;
        }

        char full_path[BRUCE_STORAGE_PATH_MAX];
        int printed = snprintf(full_path, sizeof(full_path), "/apps/%s", storage_entries[i].name);
        if (printed < 0 || (size_t)printed >= sizeof(full_path)) {
            continue;
        }

        const char *json = manifest__inspect_path(full_path);
        if (json == NULL) {
            continue;
        }

        bruce_manifest_t *manifest = manifest__parse(json, strlen(json));
        free((void *)json);
        if (manifest == NULL) {
            continue;
        }

        char label[BRUCE_LAUNCHER_LABEL_MAX];
        int label_len = snprintf(label, sizeof(label), "%s", manifest->app_name);
        if (label_len > 0 && (size_t)label_len < sizeof(label)) {
            (void)bruce_launcher__add_app(entries, &added, capacity, full_path, label);
        }
        memory__free(manifest);
    }

    return added;
}

static int bruce_launcher__items_per_page(void)
{
    int h = display__height();
    int available = h - BRUCE_LAUNCHER_TITLE_H - BRUCE_LAUNCHER_FOOTER_H;
    if (available < BRUCE_LAUNCHER_ITEM_H) {
        return 1;
    }
    return available / BRUCE_LAUNCHER_ITEM_H;
}

static void bruce_launcher__draw_entry(const bruce_launcher_entry_t *entry, int y, bool selected)
{
    int w = display__width();
    int label_max = (w - 4) / BRUCE_LAUNCHER_CHAR_W;
    char label[BRUCE_LAUNCHER_LABEL_MAX];
    if (label_max < 1) {
        label[0] = '\0';
    } else if ((int)strlen(entry->label) > label_max) {
        snprintf(label, sizeof(label), "%.*s...", label_max - 3, entry->label);
    } else {
        snprintf(label, sizeof(label), "%s", entry->label);
    }

    if (selected) {
        display__fill_rect(0, y, w, BRUCE_LAUNCHER_ITEM_H, BRUCE_COLOR_WHITE);
        display__set_text_color(BRUCE_COLOR_BLACK);
    } else {
        display__set_text_color(BRUCE_COLOR_WHITE);
    }

    display__set_text_size(BRUCE_LAUNCHER_TEXT_SIZE);
    display__set_cursor(2, y + 1);
    display__print(label);
}

static void bruce_launcher__draw_menu(const bruce_launcher_entry_t *entries, int entry_count,
                                      int selected, int first_visible)
{
    int w = display__width();
    int h = display__height();
    int items_per_page = bruce_launcher__items_per_page();
    int last_visible = first_visible + items_per_page - 1;
    if (last_visible >= entry_count) {
        last_visible = entry_count - 1;
    }

    display__clear();

    /* Title bar. */
    display__fill_rect(0, 0, w, BRUCE_LAUNCHER_TITLE_H, BRUCE_COLOR_BLUE);
    display__set_text_color(BRUCE_COLOR_WHITE);
    display__set_text_size(BRUCE_LAUNCHER_TEXT_SIZE);
    const char *title = "Bruce Launcher";
    int title_w = strlen(title) * BRUCE_LAUNCHER_CHAR_W;
    int title_x = (w - title_w) / 2;
    if (title_x < 0) {
        title_x = 0;
    }
    display__set_cursor(title_x, 1);
    display__print(title);

    /* Menu items. */
    for (int i = first_visible; i <= last_visible; ++i) {
        int y = BRUCE_LAUNCHER_TITLE_H + (i - first_visible) * BRUCE_LAUNCHER_ITEM_H;
        bruce_launcher__draw_entry(&entries[i], y, i == selected);
    }

    /* Scroll indicators. */
    display__set_text_color(BRUCE_COLOR_WHITE);
    if (first_visible > 0) {
        display__set_cursor(w - BRUCE_LAUNCHER_CHAR_W, BRUCE_LAUNCHER_TITLE_H + 1);
        display__print("^");
    }
    if (last_visible < entry_count - 1) {
        display__set_cursor(w - BRUCE_LAUNCHER_CHAR_W, h - BRUCE_LAUNCHER_FOOTER_H - BRUCE_LAUNCHER_CHAR_H - 1);
        display__print("v");
    }

    /* Footer. */
    display__fill_rect(0, h - BRUCE_LAUNCHER_FOOTER_H, w, BRUCE_LAUNCHER_FOOTER_H, BRUCE_COLOR_DARKGREY);
    display__set_text_color(BRUCE_COLOR_WHITE);
    display__set_cursor(2, h - BRUCE_LAUNCHER_FOOTER_H + 1);
    display__print("UP/DOWN/SEL to navigate");

    display__flush();
}

static int bruce_launcher__run_entry(const bruce_launcher_entry_t *entry)
{
    int result;
    if (entry->is_path) {
        result = app_runner__run_path(entry->path, "--gui", false);
    } else {
        result = app_runner__run(entry->app_name, "--gui", false);
    }

    if (result < 0) {
        char message[128];
        snprintf(message, sizeof(message), "Could not start %s (%d)", entry->label, result);
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Launch failed", message);
    }
    return result;
}

int bruce_launcher_app_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bruce_launcher_entry_t *entries = calloc(BRUCE_LAUNCHER_MAX_ENTRIES, sizeof(*entries));
    if (entries == NULL) {
        return BRUCE_ERR_NO_MEMORY;
    }
    int entry_count = 0;

    /* Feature modules live under src/modules/ and are registered as built-in
     * commands.  The launcher is only menu composition: it does not contain any
     * feature logic. */
    (void)bruce_launcher__add_builtin(entries, &entry_count, BRUCE_LAUNCHER_MAX_ENTRIES, "wifi", "Wi-Fi");
    (void)bruce_launcher__add_builtin(entries, &entry_count, BRUCE_LAUNCHER_MAX_ENTRIES, "selftest", "Self-test");
    (void)bruce_launcher__add_builtin(entries, &entry_count, BRUCE_LAUNCHER_MAX_ENTRIES, "terminal", "Terminal");

    entry_count += bruce_launcher__discover_apps(&entries[entry_count], BRUCE_LAUNCHER_MAX_ENTRIES - entry_count);

    int exit_index = entry_count;
    (void)bruce_launcher__add_builtin(entries, &entry_count, BRUCE_LAUNCHER_MAX_ENTRIES, "", "Exit");

    bruce_dialog_choice_t choices[BRUCE_LAUNCHER_MAX_ENTRIES];
    for (int i = 0; i < entry_count; ++i) {
        choices[i].label = entries[i].label;
        choices[i].value = entries[i].is_path ? entries[i].path : entries[i].app_name;
    }

    int selected = 0;
    int first_visible = 0;
    for (;;) {
        int items_per_page = bruce_launcher__items_per_page();
        if (selected < first_visible) {
            first_visible = selected;
        } else if (selected >= first_visible + items_per_page) {
            first_visible = selected - items_per_page + 1;
        }
        if (first_visible < 0) {
            first_visible = 0;
        }
        if (first_visible >= entry_count && entry_count > 0) {
            first_visible = entry_count - 1;
        }

        bruce_launcher__draw_menu(entries, entry_count, selected, first_visible);

        size_t choice = 0;
        bruce_result_t choice_result = dialog__choice("Bruce Launcher", "Select an app", choices, (size_t)entry_count,
                                                      &choice);

        if (choice_result == BRUCE_ERR_CANCELLED) {
            break;
        }
        if (choice_result != BRUCE_OK) {
            printf("Invalid choice (%d), try again.\n", choice_result);
            continue;
        }
        selected = (int)choice;
        if (selected == exit_index) {
            break;
        }
        (void)bruce_launcher__run_entry(&entries[selected]);
    }

    free(entries);
    return 0;
}
