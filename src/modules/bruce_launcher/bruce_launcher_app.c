#include "bruce_launcher_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
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
#include "core_sdk/status_icon.h"
#include "core_sdk/task.h"

#define BRUCE_LAUNCHER_CONFIG_PATH "/launcher.json"
#define BRUCE_LAUNCHER_JSON_MAX 8192
#define BRUCE_LAUNCHER_MAX_ENTRIES 32
#define BRUCE_LAUNCHER_MAX_DEPTH 4
#define BRUCE_LAUNCHER_LABEL_MAX 80
#define BRUCE_LAUNCHER_TITLE "Main Menu"
#define BRUCE_LAUNCHER_VERSION_TEXT "BRUCE"

/* MainMenu visual-style constants. */
#define BRUCE_LAUNCHER_BORDER_PAD 5
#define BRUCE_LAUNCHER_STATUS_H 25
#define BRUCE_LAUNCHER_SUBMENU_VISIBLE 3
#define BRUCE_LAUNCHER_FONT_SMALL 1
#define BRUCE_LAUNCHER_FONT_MEDIUM 2
#define BRUCE_LAUNCHER_MENU_MARGIN_X_NUM 1
#define BRUCE_LAUNCHER_MENU_MARGIN_X_DEN 10
#define BRUCE_LAUNCHER_MENU_WIDTH_NUM 8
#define BRUCE_LAUNCHER_MENU_WIDTH_DEN 10
#define BRUCE_LAUNCHER_TASKS_APP "__tasks"

/* Default launcher configuration written when /launcher.json is missing. */
static const char *BRUCE_LAUNCHER_DEFAULT_JSON =
    "{\n"
    "  \"WiFi\": {\n"
    "    \"Connect to Wifi\": \"wifi connect\",\n"
    "    \"Start WiFi AP\": \"wifi ap start\",\n"
    "    \"Turn Off WiFi\": \"wifi disconnect\",\n"
    "    \"AP info\": \"wifi ap info\",\n"
    "    \"Wifi Atks\": {\n"
    "      \"Target Atks\": \"wifiatks target\",\n"
    "      \"Karma Attack\": \"wifiatks karma\",\n"
    "      \"Beacon SPAM\": \"wifiatks beacon\"\n"
    "    }\n"
    "  },\n"
    "  \"Selftest\": \"selftest\",\n"
    "  \"Clock\": \"clock\",\n"
    "  \"Config\": {\n"
    "    \"Display & UI\": \"config display\",\n"
    "    \"LED Config\": \"config led\",\n"
    "    \"Audio Config\": \"config audio\",\n"
    "    \"System Config\": \"config system\",\n"
    "    \"Power\": \"config power\",\n"
    "    \"Install App Store\": \"appstore install\",\n"
    "    \"About\": \"config about\"\n"
    "  },\n"
    "  \"Apps\": \"/apps\"\n"
    "}\n";

/* One entry in the launcher menu. */
typedef struct bruce_launcher_menu bruce_launcher_menu_t;

typedef enum {
    BRUCE_LAUNCHER_ENTRY_COMMAND,
    BRUCE_LAUNCHER_ENTRY_SUBMENU,
    BRUCE_LAUNCHER_ENTRY_BACK,
} bruce_launcher_entry_kind_t;

typedef struct {
    char label[BRUCE_LAUNCHER_LABEL_MAX];
    bruce_launcher_entry_kind_t kind;
    char command[BRUCE_STORAGE_PATH_MAX];
    bruce_launcher_menu_t *submenu;
} bruce_launcher_entry_t;

struct bruce_launcher_menu {
    char title[BRUCE_LAUNCHER_LABEL_MAX];
    bruce_launcher_entry_t *entries;
    int entry_count;
    int capacity;
    bruce_launcher_menu_t *parent;
};

/* Theme colors cached from bruce.json. */
typedef struct {
    uint16_t pri;
    uint16_t sec;
    uint16_t bg;
} bruce_launcher_theme_t;

typedef enum {
    BRUCE_LAUNCHER_ICON_COMMAND,
    BRUCE_LAUNCHER_ICON_FOLDER,
    BRUCE_LAUNCHER_ICON_WIFI,
    BRUCE_LAUNCHER_ICON_APPS,
    BRUCE_LAUNCHER_ICON_CONFIG,
    BRUCE_LAUNCHER_ICON_CLOCK,
    BRUCE_LAUNCHER_ICON_SELFTEST,
    BRUCE_LAUNCHER_ICON_BACK,
} bruce_launcher_icon_t;

/* -------------------------------------------------------------------------- */
/* Menu tree helpers                                                          */
/* -------------------------------------------------------------------------- */

static bruce_launcher_menu_t *bruce_launcher__menu_create(const char *title, bruce_launcher_menu_t *parent)
{
    bruce_launcher_menu_t *menu = (bruce_launcher_menu_t *)calloc(1, sizeof(*menu));
    if (menu == NULL) {
        return NULL;
    }

    strncpy(menu->title, title, sizeof(menu->title) - 1);
    menu->capacity = BRUCE_LAUNCHER_MAX_ENTRIES;
    menu->entries = (bruce_launcher_entry_t *)calloc((size_t)menu->capacity, sizeof(*menu->entries));
    if (menu->entries == NULL) {
        free(menu);
        return NULL;
    }
    menu->parent = parent;
    return menu;
}

static void bruce_launcher__menu_free(bruce_launcher_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    for (int i = 0; i < menu->entry_count; ++i) {
        if (menu->entries[i].kind == BRUCE_LAUNCHER_ENTRY_SUBMENU) {
            bruce_launcher__menu_free(menu->entries[i].submenu);
            menu->entries[i].submenu = NULL;
        }
    }
    free(menu->entries);
    free(menu);
}

static bool bruce_launcher__menu_add_command(bruce_launcher_menu_t *menu, const char *label, const char *command)
{
    if (menu->entry_count >= menu->capacity) {
        return false;
    }
    bruce_launcher_entry_t *entry = &menu->entries[menu->entry_count++];
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    strncpy(entry->command, command, sizeof(entry->command) - 1);
    entry->kind = BRUCE_LAUNCHER_ENTRY_COMMAND;
    return true;
}

static bool bruce_launcher__menu_add_submenu(bruce_launcher_menu_t *menu, const char *label,
                                             bruce_launcher_menu_t *submenu)
{
    if (menu->entry_count >= menu->capacity) {
        return false;
    }
    bruce_launcher_entry_t *entry = &menu->entries[menu->entry_count++];
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    entry->submenu = submenu;
    entry->kind = BRUCE_LAUNCHER_ENTRY_SUBMENU;
    return true;
}

static bool bruce_launcher__menu_add_back(bruce_launcher_menu_t *menu)
{
    if (menu->entry_count >= menu->capacity) {
        return false;
    }
    bruce_launcher_entry_t *entry = &menu->entries[menu->entry_count++];
    strncpy(entry->label, "Back", sizeof(entry->label) - 1);
    entry->kind = BRUCE_LAUNCHER_ENTRY_BACK;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Default config I/O                                                         */
/* -------------------------------------------------------------------------- */

static char *bruce_launcher__read_file(const char *path)
{
    bruce_file_id_t file;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) {
        return NULL;
    }

    char *buffer = (char *)malloc(BRUCE_LAUNCHER_JSON_MAX);
    if (buffer == NULL) {
        storage__close(file);
        return NULL;
    }

    size_t total = 0;
    for (;;) {
        size_t chunk = 0;
        result = storage__read(file, buffer + total, BRUCE_LAUNCHER_JSON_MAX - total - 1, &chunk);
        if (result != BRUCE_OK || chunk == 0) {
            break;
        }
        total += chunk;
        if (total >= BRUCE_LAUNCHER_JSON_MAX - 1) {
            break;
        }
    }
    buffer[total] = '\0';
    storage__close(file);
    return buffer;
}

static bool bruce_launcher__write_default_config(const char *path)
{
    bruce_file_id_t file;
    bruce_result_t result = storage__open(path,
                                          BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE |
                                              BRUCE_STORAGE_OPEN_TRUNCATE,
                                          &file);
    if (result != BRUCE_OK) {
        return false;
    }

    size_t to_write = strlen(BRUCE_LAUNCHER_DEFAULT_JSON);
    size_t written = 0;
    result = storage__write(file, BRUCE_LAUNCHER_DEFAULT_JSON, to_write, &written);
    storage__close(file);
    return result == BRUCE_OK && written == to_write;
}

/* -------------------------------------------------------------------------- */
/* App discovery                                                              */
/* -------------------------------------------------------------------------- */

/* Discovers applications in a directory by extracting their manifest metadata.
 * Files without a manifest are skipped; the universal manifest__inspect_path()
 * auto-detects the format (ELF section, JS comment block, etc.) so the
 * launcher does not need format-specific knowledge. */
static int bruce_launcher__discover_apps(bruce_launcher_menu_t *menu, const char *path)
{
    bruce_storage_entry_t *storage_entries =
        (bruce_storage_entry_t *)malloc(sizeof(*storage_entries) * BRUCE_LAUNCHER_MAX_ENTRIES);
    if (storage_entries == NULL) {
        return 0;
    }

    size_t discovered_count = 0;
    bruce_result_t list_result = storage__list(path, storage_entries, BRUCE_LAUNCHER_MAX_ENTRIES, &discovered_count);
    if (list_result != BRUCE_OK) {
        free(storage_entries);
        return 0;
    }

    int added = 0;
    for (size_t i = 0; i < discovered_count && menu->entry_count < menu->capacity; ++i) {
        if (storage_entries[i].type != BRUCE_STORAGE_ENTRY_FILE) {
            continue;
        }

        char full_path[BRUCE_STORAGE_PATH_MAX];
        int printed = snprintf(full_path, sizeof(full_path), "%s/%s", path, storage_entries[i].name);
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

        if (bruce_launcher__menu_add_command(menu, manifest->app_name, full_path)) {
            added++;
        }
        memory__free(manifest);
    }

    free(storage_entries);
    return added;
}

/* -------------------------------------------------------------------------- */
/* JSON parser                                                                */
/* -------------------------------------------------------------------------- */

static bruce_launcher_menu_t *bruce_launcher__parse_json_object(cJSON *root, const char *title,
                                                                bruce_launcher_menu_t *parent, int depth);

static bool bruce_launcher__parse_json_value(bruce_launcher_menu_t *menu, const char *key, cJSON *value, int depth)
{
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        if (value->valuestring[0] == '/') {
            /* Directory discovery entry: create a submenu populated from the path. */
            bruce_launcher_menu_t *submenu = bruce_launcher__menu_create(key, menu);
            if (submenu == NULL) {
                return false;
            }
            (void)bruce_launcher__discover_apps(submenu, value->valuestring);
            (void)bruce_launcher__menu_add_back(submenu);
            if (!bruce_launcher__menu_add_submenu(menu, key, submenu)) {
                bruce_launcher__menu_free(submenu);
                return false;
            }
            return true;
        } else {
            return bruce_launcher__menu_add_command(menu, key, value->valuestring);
        }
    } else if (cJSON_IsObject(value) && depth < BRUCE_LAUNCHER_MAX_DEPTH) {
        bruce_launcher_menu_t *submenu = bruce_launcher__parse_json_object(value, key, menu, depth + 1);
        if (submenu == NULL) {
            return false;
        }
        if (!bruce_launcher__menu_add_submenu(menu, key, submenu)) {
            bruce_launcher__menu_free(submenu);
            return false;
        }
        return true;
    }

    /* Ignore unsupported JSON values silently. */
    return true;
}

static bruce_launcher_menu_t *bruce_launcher__parse_json_object(cJSON *root, const char *title,
                                                                bruce_launcher_menu_t *parent, int depth)
{
    bruce_launcher_menu_t *menu = bruce_launcher__menu_create(title, parent);
    if (menu == NULL) {
        return NULL;
    }

    cJSON *child;
    cJSON_ArrayForEach(child, root)
    {
        if (child->string == NULL) {
            continue;
        }
        if (!bruce_launcher__parse_json_value(menu, child->string, child, depth)) {
            break;
        }
    }

    if (parent != NULL) {
        (void)bruce_launcher__menu_add_back(menu);
    }
    return menu;
}

static bruce_launcher_menu_t *bruce_launcher__load_config(void)
{
    char *text = bruce_launcher__read_file(BRUCE_LAUNCHER_CONFIG_PATH);
    if (text == NULL) {
        (void)bruce_launcher__write_default_config(BRUCE_LAUNCHER_CONFIG_PATH);
        text = bruce_launcher__read_file(BRUCE_LAUNCHER_CONFIG_PATH);
    }

    cJSON *root = NULL;
    if (text != NULL) {
        root = cJSON_Parse(text);
    }
    free(text);

    if (root != NULL && cJSON_IsObject(root)) {
        bruce_launcher_menu_t *menu = bruce_launcher__parse_json_object(root, BRUCE_LAUNCHER_TITLE, NULL, 0);
        cJSON_Delete(root);
        if (menu != NULL) {
            return menu;
        }
    } else if (root != NULL) {
        cJSON_Delete(root);
    }

    /* Malformed /launcher.json: fall back to the default tree in memory. */
    root = cJSON_Parse(BRUCE_LAUNCHER_DEFAULT_JSON);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }

    bruce_launcher_menu_t *menu = bruce_launcher__parse_json_object(root, BRUCE_LAUNCHER_TITLE, NULL, 0);
    cJSON_Delete(root);
    return menu;
}

/* -------------------------------------------------------------------------- */
/* Visual style                                                               */
/* -------------------------------------------------------------------------- */

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

static uint32_t bruce_launcher__draw_status_icons(const bruce_launcher_theme_t *theme)
{
    bruce_status_icon_t icons[BRUCE_STATUS_ICON_MAX];
    size_t count = 0;
    uint32_t revision = 0;
    if (status_icon__list(icons, BRUCE_STATUS_ICON_MAX, &count, &revision) != BRUCE_OK) {
        return revision;
    }
    int w = display__width();
    display__fill_rect(55, 6, w - 63, BRUCE_LAUNCHER_STATUS_H - 7, theme->bg);
    display__set_text_bg_color(theme->bg);
    int x = w - BRUCE_LAUNCHER_BORDER_PAD - 2;
    for (size_t i = count; i > 0; --i) {
        const bruce_status_icon_t *icon = &icons[i - 1];
        x -= icon->width;
        if (x < 57) break;
        display__draw_bitmap((int16_t)x, (int16_t)(7 + (16 - icon->height) / 2), icon->bitmap,
                             icon->width, icon->height, theme->pri);
        x -= 3;
    }
    return revision;
}

/* Draw the main border/status bar: background fill, rounded screen border,
 * horizontal status-line separator, and "BRUCE" text in the top-left corner. */
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
    (void)bruce_launcher__draw_status_icons(theme);
}

static void bruce_launcher__draw_centered_text(const char *text, int y, int font_size,
                                                const bruce_launcher_theme_t *theme)
{
    int text_w = (int)strlen(text) * 8 * font_size;
    display__set_text_size(font_size);
    display__set_text_color(theme->pri);
    display__set_text_bg_color(theme->bg);
    display__set_cursor((display__width() - text_w) / 2, y);
    display__print(text);
}

static bruce_launcher_icon_t bruce_launcher__entry_icon(const bruce_launcher_entry_t *entry)
{
    if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) {
        return BRUCE_LAUNCHER_ICON_BACK;
    }
    if (strcasecmp(entry->label, "wifi") == 0) {
        return BRUCE_LAUNCHER_ICON_WIFI;
    }
    if (strcasecmp(entry->label, "apps") == 0) {
        return BRUCE_LAUNCHER_ICON_APPS;
    }
    if (strcasecmp(entry->label, "config") == 0) {
        return BRUCE_LAUNCHER_ICON_CONFIG;
    }
    if (strcasecmp(entry->label, "clock") == 0) {
        return BRUCE_LAUNCHER_ICON_CLOCK;
    }
    if (strcasecmp(entry->label, "selftest") == 0 || strcasecmp(entry->label, "self-test") == 0) {
        return BRUCE_LAUNCHER_ICON_SELFTEST;
    }
    return entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU ? BRUCE_LAUNCHER_ICON_FOLDER
                                                       : BRUCE_LAUNCHER_ICON_COMMAND;
}

static void bruce_launcher__draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    int half = thickness / 2;
    bool mostly_horizontal = abs(x1 - x0) >= abs(y1 - y0);
    for (int offset = -half; offset <= half; ++offset) {
        display__draw_line(x0 + (mostly_horizontal ? 0 : offset),
                           y0 + (mostly_horizontal ? offset : 0),
                           x1 + (mostly_horizontal ? 0 : offset),
                           y1 + (mostly_horizontal ? offset : 0), color);
    }
}

static void bruce_launcher__draw_arc_band(int cx, int cy, int outer_radius, int inner_radius,
                                           int start_angle, int end_angle, uint16_t color)
{
    for (int radius = inner_radius; radius <= outer_radius; ++radius) {
        display__draw_arc(cx, cy, radius, start_angle, end_angle, color);
    }
}

static void bruce_launcher__draw_entry_icon(const bruce_launcher_entry_t *entry, int cx, int cy,
                                             int size, uint16_t color)
{
    bruce_launcher_icon_t icon = bruce_launcher__entry_icon(entry);
    int left = cx - size / 2;
    int top = cy - size / 2;
    int pad = size >= 48 ? 5 : 3;
    int stroke = size >= 48 ? 3 : 1;

    if (icon == BRUCE_LAUNCHER_ICON_WIFI) {
        int delta_y = size * 20 / 64;
        int dot_radius = size * 6 / 64;
        int origin_y = cy + delta_y;
        display__fill_circle(cx, origin_y, dot_radius, color);
        bruce_launcher__draw_arc_band(cx, origin_y, delta_y + dot_radius, delta_y,
                                      130, 230, color);
        bruce_launcher__draw_arc_band(cx, origin_y, 2 * delta_y + dot_radius, 2 * delta_y,
                                      130, 230, color);
    } else if (icon == BRUCE_LAUNCHER_ICON_APPS) {
        /* Legacy Scripts document with a folded corner and code mark. */
        int page_w = size * 40 / 64;
        int page_h = size * 60 / 64;
        int fold = page_h / 4;
        int page_x = cx - page_w / 2;
        int page_y = cy - page_h / 2;
        bruce_launcher__draw_thick_line(page_x, page_y, page_x + page_w - fold, page_y, stroke, color);
        bruce_launcher__draw_thick_line(page_x + page_w - fold, page_y,
                                        page_x + page_w, page_y + fold, stroke, color);
        bruce_launcher__draw_thick_line(page_x + page_w, page_y + fold,
                                        page_x + page_w, page_y + page_h, stroke, color);
        bruce_launcher__draw_thick_line(page_x + page_w, page_y + page_h,
                                        page_x, page_y + page_h, stroke, color);
        bruce_launcher__draw_thick_line(page_x, page_y + page_h, page_x, page_y, stroke, color);
        bruce_launcher__draw_thick_line(page_x + page_w - fold, page_y,
                                        page_x + page_w - fold, page_y + fold, stroke, color);
        bruce_launcher__draw_thick_line(page_x + page_w - fold, page_y + fold,
                                        page_x + page_w, page_y + fold, stroke, color);

        int mark_y = cy + page_h / 8;
        int mark_dx = page_w / 5;
        int mark_dy = page_h / 10;
        bruce_launcher__draw_thick_line(cx - mark_dx / 2, mark_y,
                                        cx - mark_dx, mark_y + mark_dy, stroke, color);
        bruce_launcher__draw_thick_line(cx - mark_dx, mark_y + mark_dy,
                                        cx - mark_dx / 2, mark_y + 2 * mark_dy, stroke, color);
        bruce_launcher__draw_thick_line(cx + mark_dx / 3, mark_y + 2 * mark_dy,
                                        cx + mark_dx, mark_y, stroke, color);
        bruce_launcher__draw_thick_line(cx + mark_dx, mark_y,
                                        cx + mark_dx * 3 / 2, mark_y + mark_dy, stroke, color);
        bruce_launcher__draw_thick_line(cx + mark_dx * 3 / 2, mark_y + mark_dy,
                                        cx + mark_dx, mark_y + 2 * mark_dy, stroke, color);
    } else if (icon == BRUCE_LAUNCHER_ICON_CONFIG) {
        int radius = size * 9 / 64;
        for (int tooth = 0; tooth < 6; ++tooth) {
            bruce_launcher__draw_arc_band(cx, cy, radius * 7 / 2, radius * 2,
                                          15 + 60 * tooth, 45 + 60 * tooth, color);
        }
        bruce_launcher__draw_arc_band(cx, cy, radius * 5 / 2, radius, 0, 360, color);
    } else if (icon == BRUCE_LAUNCHER_ICON_CLOCK) {
        int radius = size * 30 / 64;
        int pointer = size * 15 / 64;
        bruce_launcher__draw_arc_band(cx, cy, radius * 11 / 10, radius, 0, 360, color);
        bruce_launcher__draw_thick_line(cx, cy, cx - pointer * 2 / 3,
                                        cy - pointer * 2 / 3, stroke, color);
        bruce_launcher__draw_thick_line(cx, cy, cx + pointer, cy - pointer, stroke, color);
        display__fill_circle(cx, cy, stroke + 1, color);
    } else if (icon == BRUCE_LAUNCHER_ICON_SELFTEST) {
        int radius = size / 2 - pad;
        bruce_launcher__draw_arc_band(cx, cy, radius, radius - stroke + 1, 45, 315, color);
        bruce_launcher__draw_thick_line(cx - radius / 2, cy, cx - radius / 7,
                                        cy + radius / 3, stroke, color);
        bruce_launcher__draw_thick_line(cx - radius / 7, cy + radius / 3,
                                        cx + radius * 3 / 5, cy - radius / 3, stroke, color);
    } else if (icon == BRUCE_LAUNCHER_ICON_BACK) {
        bruce_launcher__draw_thick_line(left + pad, cy, left + size - pad, cy, stroke, color);
        bruce_launcher__draw_thick_line(left + pad, cy, cx, top + pad, stroke, color);
        bruce_launcher__draw_thick_line(left + pad, cy, cx, top + size - pad, stroke, color);
    } else if (icon == BRUCE_LAUNCHER_ICON_FOLDER) {
        int tab_h = size / 5;
        display__draw_round_rect(left + pad, top + tab_h, size - 2 * pad,
                                 size - tab_h - pad, size / 10, color);
        display__draw_round_rect(left + pad * 2, top + pad, size * 2 / 5,
                                 tab_h * 2, size / 12, color);
    } else {
        display__draw_round_rect(left + pad, top + pad, size - 2 * pad, size - 2 * pad,
                                 size / 10, color);
        display__fill_triangle(cx - size / 8, cy - size / 5, cx - size / 8,
                               cy + size / 5, cx + size / 5, cy, color);
    }
}

static void bruce_launcher__draw_carousel_arrow(int cx, int cy, int direction, bool compact,
                                                 uint16_t color)
{
    int extent = compact ? 8 : 11;
    int stroke = compact ? 2 : 3;
    int tip_x = cx + direction * extent / 2;
    int back_x = cx - direction * extent / 2;
    bruce_launcher__draw_thick_line(back_x, cy - extent, tip_x, cy, stroke, color);
    bruce_launcher__draw_thick_line(tip_x, cy, back_x, cy + extent, stroke, color);
}

static void bruce_launcher__draw_root_menu(const bruce_launcher_menu_t *menu, int selected,
                                            const bruce_launcher_theme_t *theme)
{
    int w = display__width();
    int h = display__height();
    if (menu->entry_count == 0) {
        bruce_launcher__draw_centered_text("No entries", (h + BRUCE_LAUNCHER_STATUS_H) / 2,
                                           BRUCE_LAUNCHER_FONT_SMALL, theme);
        return;
    }

    bool compact = w < 180 || h < 180;
    int large = compact ? 52 : 64;
    int content_h = h - BRUCE_LAUNCHER_STATUS_H;
    int cy = BRUCE_LAUNCHER_STATUS_H + content_h * 2 / 5;

    if (menu->entry_count > 1) {
        bruce_launcher__draw_carousel_arrow(w / 7, cy, -1, compact, theme->pri);
        bruce_launcher__draw_carousel_arrow(w - w / 7, cy, 1, compact, theme->pri);
    }
    bruce_launcher__draw_entry_icon(&menu->entries[selected], w / 2, cy, large, theme->pri);

    int font_size = compact ? BRUCE_LAUNCHER_FONT_SMALL : bruce_launcher__font_size(w);
    bruce_launcher__draw_centered_text(menu->entries[selected].label, cy + large / 2 + 12,
                                       font_size, theme);
}

/* Draw a centered submenu box with up to three visible rows. */
static void bruce_launcher__draw_options(const bruce_launcher_entry_t *entries, int entry_count,
                                         int selected, const char *title,
                                         const bruce_launcher_theme_t *theme)
{
    int w = display__width();
    int h = display__height();
    int font_size = bruce_launcher__font_size(w);
    int char_w = 8 * font_size;
    int char_h = 16 * font_size;
    int line_h = char_h + 2;

    int box_x = w * BRUCE_LAUNCHER_MENU_MARGIN_X_NUM / BRUCE_LAUNCHER_MENU_MARGIN_X_DEN;
    int box_w = w * BRUCE_LAUNCHER_MENU_WIDTH_NUM / BRUCE_LAUNCHER_MENU_WIDTH_DEN;

    /* Fit the menu box inside the screen and cap the visible row count. */
    int available_h = h - 2 * BRUCE_LAUNCHER_BORDER_PAD - BRUCE_LAUNCHER_STATUS_H - 10;
    if (available_h < line_h + 10) {
        available_h = line_h + 10;
    }
    int max_visible = (available_h - 10) / line_h;
    if (max_visible < 1) {
        max_visible = 1;
    }
    if (max_visible > BRUCE_LAUNCHER_SUBMENU_VISIBLE) {
        max_visible = BRUCE_LAUNCHER_SUBMENU_VISIBLE;
    }

    int visible = entry_count;
    if (visible > max_visible) {
        visible = max_visible;
    }
    if (visible < 1) {
        visible = 1;
    }

    int box_h = visible * line_h + 10;
    int box_y = (h - box_h) / 2;
    if (box_y + box_h > h - BRUCE_LAUNCHER_BORDER_PAD) {
        box_y = h - BRUCE_LAUNCHER_BORDER_PAD - box_h;
    }
    if (box_y < BRUCE_LAUNCHER_STATUS_H + 2) {
        box_y = BRUCE_LAUNCHER_STATUS_H + 2;
    }

    int title_y = box_y - char_h - 4;
    if (title_y >= BRUCE_LAUNCHER_STATUS_H + 2) {
        bruce_launcher__draw_centered_text(title, title_y, font_size, theme);
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

}

static size_t bruce_launcher__task_candidates(bruce_task_snapshot_t *tasks, size_t capacity)
{
    bruce_task_snapshot_t all[16];
    size_t count = 0;
    size_t written = 0;
    bruce_task_id_t self = task__current_id();
    if (task__list(all, sizeof(all) / sizeof(all[0]), &count) != BRUCE_OK) {
        return 0;
    }
    for (size_t i = 0; i < count && written < capacity; ++i) {
        if (all[i].id != self && all[i].gui_requested && all[i].state == BRUCE_TASK_BACKGROUND) {
            tasks[written++] = all[i];
        }
    }
    return written;
}

static void bruce_launcher__task_layout(const bruce_task_snapshot_t *tasks, size_t count,
                                         int selected, bruce_display_tile_t *tiles,
                                         const bruce_launcher_theme_t *theme)
{
    int w = display__width();
    int h = display__height();
    int top = BRUCE_LAUNCHER_STATUS_H + 2;
    int cols = (count == 1 || (count == 2 && h > w)) ? 1 : 2;
    int rows = (int)((count + (size_t)cols - 1) / (size_t)cols);
    int cell_w = w / cols;
    int cell_h = (h - top) / rows;

    display__fill_screen(theme->bg);
    display__set_text_size(1);
    display__set_text_color(theme->pri);
    display__set_text_bg_color(theme->bg);
    display__set_cursor(4, 7);
    display__print("Tasks  arrows: move  select: open  back: exit");
    display__draw_line(0, BRUCE_LAUNCHER_STATUS_H, w - 1, BRUCE_LAUNCHER_STATUS_H, theme->pri);

    for (size_t i = 0; i < count; ++i) {
        int col = (int)i % cols;
        int row = (int)i / cols;
        int cell_x = col * cell_w;
        int cell_y = top + row * cell_h;
        int right = col == cols - 1 ? w : cell_x + cell_w;
        int bottom = row == rows - 1 ? h : cell_y + cell_h;
        tiles[i].task_id = tasks[i].id;
        tiles[i].rect = (bruce_display_rect_t){
            .x = cell_x + 3,
            .y = cell_y + 12,
            .width = right - cell_x - 6,
            .height = bottom - cell_y - 15,
        };
        display__set_cursor(cell_x + 4, cell_y + 2);
        display__print(tasks[i].name);
        display__draw_rect(cell_x + 1, cell_y + 10, right - cell_x - 2,
                           bottom - cell_y - 11, (int)i == selected ? theme->pri : theme->sec);
    }
}

static bruce_result_t bruce_launcher__draw_task_page(const bruce_task_snapshot_t *tasks, size_t count,
                                                       int selected, const bruce_launcher_theme_t *theme)
{
    bruce_result_t result = display__set_tiles(NULL, 0);
    if (result != BRUCE_OK) {
        return result;
    }
    result = display__begin_frame();
    if (result != BRUCE_OK) {
        return result;
    }
    bruce_display_tile_t tiles[BRUCE_DISPLAY_MAX_TILES];
    bruce_launcher__task_layout(tasks, count, selected, tiles, theme);
    result = display__present();
    if (result != BRUCE_OK || count == 0) {
        return result;
    }
    return display__set_tiles(tiles, count);
}

static int bruce_launcher__run_task_switcher(const bruce_launcher_theme_t *theme)
{
    size_t page = 0;
    int selected = 0;
    bool redraw = true;
    for (;;) {
        bruce_task_snapshot_t candidates[16];
        size_t total = bruce_launcher__task_candidates(candidates, sizeof(candidates) / sizeof(candidates[0]));
        size_t pages = total == 0 ? 1 : (total + BRUCE_DISPLAY_MAX_TILES - 1) / BRUCE_DISPLAY_MAX_TILES;
        if (page >= pages) page = pages - 1;
        size_t start = page * BRUCE_DISPLAY_MAX_TILES;
        size_t page_count = total > start ? total - start : 0;
        if (page_count > BRUCE_DISPLAY_MAX_TILES) page_count = BRUCE_DISPLAY_MAX_TILES;
        if (selected >= (int)page_count) selected = page_count > 0 ? (int)page_count - 1 : 0;

        if (redraw) {
            bruce_result_t draw = bruce_launcher__draw_task_page(&candidates[start], page_count, selected, theme);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            redraw = false;
        }

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            return 0;
        }
        if (input_result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) {
            continue;
        }
        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B) {
            (void)display__set_tiles(NULL, 0);
            return 0;
        }
        if ((event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_LEFT) && selected > 0) {
            selected--;
            redraw = true;
        } else if ((event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_RIGHT) &&
                   selected + 1 < (int)page_count) {
            selected++;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_LEFT && page > 0) {
            page--;
            selected = 0;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_RIGHT && page + 1 < pages) {
            page++;
            selected = 0;
            redraw = true;
        } else if ((event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A) &&
                   page_count > 0) {
            bruce_task_id_t target = candidates[start + (size_t)selected].id;
            bruce_task_snapshot_t snapshot;
            if (task__snapshot(target, &snapshot) == BRUCE_OK) {
                (void)display__set_tiles(NULL, 0);
                (void)task__foreground(target);
                return 0;
            }
            redraw = true;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Command dispatch                                                           */
/* -------------------------------------------------------------------------- */

static void bruce_launcher__split_command(const char *command, char *first, size_t first_size, const char **rest)
{
    const char *p = command;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    size_t i = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && i + 1 < first_size) {
        first[i++] = *p++;
    }
    first[i] = '\0';

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *rest = p;
}

static int bruce_launcher__run_entry(const bruce_launcher_entry_t *entry)
{
    if (strcmp(entry->command, BRUCE_LAUNCHER_TASKS_APP) == 0) {
        bruce_launcher_theme_t theme;
        bruce_launcher__get_theme(&theme);
        return bruce_launcher__run_task_switcher(&theme);
    }
    int result;

    if (entry->command[0] == '/') {
        result = app_runner__run_path(entry->command, NULL, false);
    } else {
        char first[BRUCE_LAUNCHER_LABEL_MAX];
        const char *rest = NULL;
        bruce_launcher__split_command(entry->command, first, sizeof(first), &rest);
        if (first[0] == '\0') {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        result = app_runner__run(first, rest[0] != '\0' ? rest : NULL, false);
    }

    if (result < 0) {
        char message[128];
        snprintf(message, sizeof(message), "Could not start %s (%d)", entry->label, result);
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Launch failed", message);
    }
    return result;
}

/* -------------------------------------------------------------------------- */
/* GUI menu runner                                                            */
/* -------------------------------------------------------------------------- */

/* The root is a horizontal carousel. Nested menus are vertical three-row
 * lists. SELECT/Btn-A opens the highlighted entry and BACK/Btn-B returns. */
static int bruce_launcher__run_gui_menu(bruce_launcher_menu_t *menu)
{
    bruce_launcher_theme_t theme;
    bruce_launcher__get_theme(&theme);

    (void)runtime__delay(300);
    (void)input__flush();

    int selected = 0;
    int last_drawn = -1;
    uint32_t icon_revision = UINT32_MAX;
    for (;;) {
        if (selected != last_drawn) {
            bruce_result_t frame = display__begin_frame();
            if (frame == BRUCE_ERR_NOT_FOREGROUND) {
                (void)runtime__delay(20);
                continue;
            }
            if (frame != BRUCE_OK) {
                return frame;
            }
            bruce_launcher__draw_main_border(&theme);
            if (menu->parent == NULL) {
                bruce_launcher__draw_root_menu(menu, selected, &theme);
            } else {
                bruce_launcher__draw_options(menu->entries, menu->entry_count, selected, menu->title, &theme);
            }
            icon_revision = bruce_launcher__draw_status_icons(&theme);
            frame = display__present();
            if (frame != BRUCE_OK) {
                return frame;
            }
            last_drawn = selected;
        }

        size_t icon_count = 0;
        uint32_t current_revision = 0;
        if (status_icon__list(NULL, 0, &icon_count, &current_revision) == BRUCE_OK &&
            current_revision != icon_revision) {
            bruce_result_t frame = display__begin_frame();
            if (frame == BRUCE_OK) {
                icon_revision = bruce_launcher__draw_status_icons(&theme);
                (void)display__present();
            }
        }

        bruce_input_event_t ev;
        bruce_result_t result = input__read(&ev, 100);
        if (result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) {
            continue;
        }

        switch (ev.code) {
            case BRUCE_INPUT_CODE_LEFT:
            case BRUCE_INPUT_CODE_UP:
                if (menu->parent == NULL && menu->entry_count > 0) {
                    selected = (selected + menu->entry_count - 1) % menu->entry_count;
                } else if (selected > 0) {
                    selected--;
                }
                break;
            case BRUCE_INPUT_CODE_RIGHT:
            case BRUCE_INPUT_CODE_DOWN:
                if (menu->parent == NULL && menu->entry_count > 0) {
                    selected = (selected + 1) % menu->entry_count;
                } else if (selected + 1 < menu->entry_count) {
                    selected++;
                }
                break;
            case BRUCE_INPUT_CODE_SELECT:
            case BRUCE_INPUT_CODE_BUTTON_A: {
                if (menu->entry_count == 0) {
                    break;
                }
                const bruce_launcher_entry_t *entry = &menu->entries[selected];
                if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) {
                    return 0;
                }
                if (entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU) {
                    (void)bruce_launcher__run_gui_menu(entry->submenu);
                } else {
                    (void)bruce_launcher__run_entry(entry);
                }
                (void)input__flush();
                last_drawn = -1;
                break;
            }
            case BRUCE_INPUT_CODE_BACK:
            case BRUCE_INPUT_CODE_BUTTON_B:
                return 0;
            default:
                break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Terminal menu runner                                                       */
/* -------------------------------------------------------------------------- */

/* Terminal fallback used when the launcher is started without --gui. Keeps
 * the original renderer-agnostic dialog__choice() path so serial/terminal
 * usage and the host selftest continue to work. */
static int bruce_launcher__run_terminal_menu(bruce_launcher_menu_t *menu)
{
    bruce_dialog_choice_t *choices =
        (bruce_dialog_choice_t *)malloc(sizeof(*choices) * (size_t)menu->capacity);
    if (choices == NULL) {
        return BRUCE_ERR_NO_MEMORY;
    }

    for (;;) {
        for (int i = 0; i < menu->entry_count; ++i) {
            choices[i].label = menu->entries[i].label;
            choices[i].value = menu->entries[i].label;
        }

        size_t choice = 0;
        bruce_result_t choice_result =
            dialog__choice(menu->title, "Select an app", choices, (size_t)menu->entry_count, &choice);

        if (choice_result == BRUCE_ERR_CANCELLED) {
            break;
        }
        if (choice_result != BRUCE_OK) {
            printf("Invalid choice (%d), try again.\n", choice_result);
            continue;
        }

        const bruce_launcher_entry_t *entry = &menu->entries[(int)choice];
        if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) {
            break;
        }
        if (entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU) {
            (void)bruce_launcher__run_terminal_menu(entry->submenu);
        } else {
            (void)bruce_launcher__run_entry(entry);
        }
    }

    free(choices);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

int bruce_launcher_app_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bruce_launcher_menu_t *root = bruce_launcher__load_config();
    if (root == NULL) {
        printf("Failed to load launcher configuration\n");
        return BRUCE_ERR_INTERNAL;
    }

    int result;
    if (app_runner__args_have_gui(argc, argv)) {
        result = bruce_launcher__run_gui_menu(root);
    } else {
        result = bruce_launcher__run_terminal_menu(root);
    }

    bruce_launcher__menu_free(root);
    return result;
}
