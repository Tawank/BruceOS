#include "bruce_launcher_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"
#include "core_sdk/task.h"

#define BRUCE_LAUNCHER_MAX_ENTRIES 32
#define BRUCE_LAUNCHER_LABEL_MAX 80
#define BRUCE_LAUNCHER_TITLE "Main Menu"
#define BRUCE_LAUNCHER_VERSION_TEXT "BRUCE"

/* Legacy MainMenu look constants. The original Arduino UI centers the menu in
 * a rounded rectangle that occupies 80% of the screen width, with the selected
 * item highlighted in the primary theme color and a "BRUCE" label in the
 * status bar. */
#define BRUCE_LAUNCHER_BORDER_PAD 5
#define BRUCE_LAUNCHER_STATUS_H 25
#define BRUCE_LAUNCHER_MAX_VISIBLE 6
#define BRUCE_LAUNCHER_FONT_SMALL 1
#define BRUCE_LAUNCHER_FONT_MEDIUM 2
#define BRUCE_LAUNCHER_FONT_BIG 3
#define BRUCE_LAUNCHER_MENU_MARGIN_X_NUM 1
#define BRUCE_LAUNCHER_MENU_MARGIN_X_DEN 10
#define BRUCE_LAUNCHER_MENU_WIDTH_NUM 8
#define BRUCE_LAUNCHER_MENU_WIDTH_DEN 10

/* One entry in the launcher menu.  Built-ins are dispatched by name; /apps/
 * entries are dispatched by path. */
typedef struct {
    char label[BRUCE_LAUNCHER_LABEL_MAX];
    char app_name[BRUCE_STORAGE_NAME_MAX];
    char path[BRUCE_STORAGE_PATH_MAX];
    bool is_path;
} bruce_launcher_entry_t;

/* Theme colors cached from bruce.json. */
typedef struct {
    uint16_t pri;
    uint16_t sec;
    uint16_t bg;
} bruce_launcher_theme_t;

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

static void bruce_launcher__get_theme(bruce_launcher_theme_t *theme)
{
    if (config__get_pri_color(&theme->pri) != BRUCE_OK) {
        theme->pri = BRUCE_COLOR_WHITE;
    }
    if (config__get_sec_color(&theme->sec) != BRUCE_OK) {
        theme->sec = BRUCE_COLOR_LIGHTGREY;
    }
    if (config__get_bg_color(&theme->bg) != BRUCE_OK) {
        theme->bg = BRUCE_COLOR_BLACK;
    }
}

static int bruce_launcher__font_size(int w)
{
    /* Use the medium font on wide (landscape) screens, small font on the
     * 135x240 portrait reference. */
    return (w >= 200) ? BRUCE_LAUNCHER_FONT_MEDIUM : BRUCE_LAUNCHER_FONT_SMALL;
}

/* Draw the main border/status bar exactly like the legacy MainMenu:
 * background fill, rounded screen border, horizontal status-line separator,
 * and "BRUCE" text in the top-left corner. */
static void bruce_launcher__draw_main_border(const bruce_launcher_theme_t *theme)
{
    int w = display__width();
    int h = display__height();

    display__fill_screen(theme->bg);
    display__draw_round_rect(BRUCE_LAUNCHER_BORDER_PAD, BRUCE_LAUNCHER_BORDER_PAD,
                             w - 2 * BRUCE_LAUNCHER_BORDER_PAD,
                             h - 2 * BRUCE_LAUNCHER_BORDER_PAD,
                             5, theme->pri);
    display__draw_line(BRUCE_LAUNCHER_BORDER_PAD, BRUCE_LAUNCHER_STATUS_H,
                       w - BRUCE_LAUNCHER_BORDER_PAD, BRUCE_LAUNCHER_STATUS_H, theme->pri);

    display__set_text_color(theme->pri);
    display__set_text_bg_color(theme->bg);
    display__set_text_size(BRUCE_LAUNCHER_FONT_SMALL);
    display__set_cursor(BRUCE_LAUNCHER_BORDER_PAD + 2, 7);
    display__print(BRUCE_LAUNCHER_VERSION_TEXT);
}

/* Draw the centered rounded menu box that loopOptions/drawOptions used in the
 * legacy Arduino code. Selected row is highlighted in the primary color with a
 * ">" prefix; other rows are prefixed with a space. */
static void bruce_launcher__draw_options(const bruce_launcher_entry_t *entries, int entry_count,
                                         int selected, const bruce_launcher_theme_t *theme)
{
    int w = display__width();
    int h = display__height();
    int font_size = bruce_launcher__font_size(w);
    int char_w = 8 * font_size;
    int char_h = 16 * font_size;
    int line_h = char_h + 2;

    int visible = entry_count;
    if (visible > BRUCE_LAUNCHER_MAX_VISIBLE) {
        visible = BRUCE_LAUNCHER_MAX_VISIBLE;
    }

    int box_x = w * BRUCE_LAUNCHER_MENU_MARGIN_X_NUM / BRUCE_LAUNCHER_MENU_MARGIN_X_DEN;
    int box_w = w * BRUCE_LAUNCHER_MENU_WIDTH_NUM / BRUCE_LAUNCHER_MENU_WIDTH_DEN;
    int box_h = visible * line_h + 10;
    int box_y = (h - box_h) / 2;
    if (box_y < BRUCE_LAUNCHER_STATUS_H + 2) {
        box_y = BRUCE_LAUNCHER_STATUS_H + 2;
    }

    /* Background box with border. */
    display__fill_round_rect(box_x, box_y, box_w, box_h, 5, theme->bg);
    display__draw_round_rect(box_x, box_y, box_w, box_h, 5, theme->pri);

    /* Scroll window so the selection is always visible. */
    int first = selected - visible / 2;
    if (first < 0) {
        first = 0;
    }
    if (first > entry_count - visible) {
        first = entry_count - visible;
    }
    if (first < 0) {
        first = 0;
    }

    int max_chars = (box_w - 10) / char_w - 1;
    if (max_chars < 1) {
        max_chars = 1;
    }

    for (int i = first; i < first + visible && i < entry_count; ++i) {
        int y = box_y + 5 + (i - first) * line_h;
        bool is_selected = (i == selected);

        char label[BRUCE_LAUNCHER_LABEL_MAX];
        if ((int)strlen(entries[i].label) > max_chars) {
            snprintf(label, sizeof(label), "%.*s...", max_chars - 3, entries[i].label);
        } else {
            snprintf(label, sizeof(label), "%s", entries[i].label);
        }

        if (is_selected) {
            display__fill_round_rect(box_x + 2, y + 1, box_w - 4, line_h - 2, 3, theme->pri);
            display__set_text_color(theme->bg);
            display__set_text_bg_color(theme->pri);
        } else {
            display__set_text_color(theme->pri);
            display__set_text_bg_color(theme->bg);
        }

        display__set_text_size(font_size);
        display__set_cursor(box_x + 5, y + 2);

        char text[BRUCE_LAUNCHER_LABEL_MAX + 4];
        snprintf(text, sizeof(text), "%c%s", is_selected ? '>' : ' ', label);
        display__print(text);
    }

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

/* GUI menu loop that mirrors the legacy MainMenu interaction: UP/DOWN move the
 * selection, SELECT/ Btn-A launches the highlighted item, BACK/Btn-B exits.
 * A short initial delay avoids immediately selecting if the button is still
 * held from the previous screen. */
static int bruce_launcher__run_legacy_menu(bruce_launcher_entry_t *entries, int entry_count, int exit_index)
{
    bruce_launcher_theme_t theme;
    bruce_launcher__get_theme(&theme);

    (void)runtime__delay(300);
    (void)input__flush();

    bruce_launcher__draw_main_border(&theme);

    int selected = 0;
    int last_drawn = -1;
    for (;;) {
        if (selected != last_drawn) {
            bruce_launcher__draw_options(entries, entry_count, selected, &theme);
            last_drawn = selected;
        }

        bruce_input_event_t ev;
        bruce_result_t result = input__read(&ev, 100);
        if (result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) {
            continue;
        }

        switch (ev.code) {
            case BRUCE_INPUT_CODE_UP:
                if (selected > 0) {
                    selected--;
                }
                break;
            case BRUCE_INPUT_CODE_DOWN:
                if (selected + 1 < entry_count) {
                    selected++;
                }
                break;
            case BRUCE_INPUT_CODE_SELECT:
            case BRUCE_INPUT_CODE_BUTTON_A:
                if (selected == exit_index) {
                    return 0;
                }
                (void)bruce_launcher__run_entry(&entries[selected]);
                (void)input__flush();
                bruce_launcher__draw_main_border(&theme);
                last_drawn = -1;
                break;
            case BRUCE_INPUT_CODE_BACK:
            case BRUCE_INPUT_CODE_BUTTON_B:
                return 0;
            default:
                break;
        }
    }
}

/* Terminal fallback used when the launcher is started without --gui. Keeps
 * the original renderer-agnostic dialog__choice() path so serial/terminal
 * usage and the host selftest continue to work. */
static int bruce_launcher__run_terminal_menu(bruce_launcher_entry_t *entries, int entry_count, int exit_index)
{
    bruce_dialog_choice_t choices[BRUCE_LAUNCHER_MAX_ENTRIES];
    for (int i = 0; i < entry_count; ++i) {
        choices[i].label = entries[i].label;
        choices[i].value = entries[i].is_path ? entries[i].path : entries[i].app_name;
    }

    for (;;) {
        size_t choice = 0;
        bruce_result_t choice_result = dialog__choice(BRUCE_LAUNCHER_TITLE, "Select an app", choices,
                                                        (size_t)entry_count, &choice);

        if (choice_result == BRUCE_ERR_CANCELLED) {
            break;
        }
        if (choice_result != BRUCE_OK) {
            printf("Invalid choice (%d), try again.\n", choice_result);
            continue;
        }
        int selected = (int)choice;
        if (selected == exit_index) {
            break;
        }
        (void)bruce_launcher__run_entry(&entries[selected]);
    }

    return 0;
}

int bruce_launcher_app_main(int argc, char **argv)
{
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

    int result;
    if (app_runner__args_have_gui(argc, argv)) {
        result = bruce_launcher__run_legacy_menu(entries, entry_count, exit_index);
    } else {
        result = bruce_launcher__run_terminal_menu(entries, entry_count, exit_index);
    }

    free(entries);
    return result;
}
