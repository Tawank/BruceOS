#include "system_menu_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "core_sdk/app_config.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/display.h"
#include "core_sdk/icon.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define SYSTEM_MENU__APP_NAME "menu"
#define SYSTEM_MENU__MAX_ITEMS 8
#define SYSTEM_MENU__TEXT_MAX 32
#define SYSTEM_MENU__ACTION_MAX 64
#define SYSTEM_MENU__CONFIG_MAX 1024
#define SYSTEM_MENU__ROW_H 42
#define SYSTEM_MENU__ICON_SIZE 20
#define SYSTEM_MENU__BAR_X 34
#define SYSTEM_MENU__BAR_RIGHT 40
#define SYSTEM_MENU__BAR_STEP 10

/* Items whose action is "volume" or "brightness" are drawn as a slider filling
 * a row of its own; every run of other items shares one row of buttons. */
static const char *const SYSTEM_MENU__DEFAULT_ITEMS_JSON =
    "["
    "{\"icon\":\"cancel\",\"label\":\"Cancel\",\"action\":\"cancel\"},"
    "{\"icon\":\"keyboard-esc\",\"label\":\"Esc\",\"action\":\"input.esc\"},"
    "{\"icon\":\"keyboard-return\",\"label\":\"Enter\",\"action\":\"input.enter\"},"
    "{\"icon\":\"arrange-bring-forward\",\"label\":\"Switch\",\"action\":\"process.preview\"},"
    "{\"icon\":\"apps\",\"label\":\"Launcher\",\"action\":\"launcher\"},"
    "{\"icon\":\"power\",\"label\":\"Off\",\"action\":\"shutdown now\"}"
    "]";

static const char *const SYSTEM_MENU__DEFAULT_EPAPER_ITEMS_JSON =
    "["
    "{\"icon\":\"volume-high\",\"action\":\"volume\"},"
    "{\"icon\":\"brightness\",\"action\":\"brightness\"},"
    "{\"icon\":\"dither\",\"label\":\"Dither\",\"action\":\"display.dither\"},"
    "{\"icon\":\"rotate\",\"label\":\"Rotate\",\"action\":\"display.rotate\"},"
    "{\"icon\":\"sleep\",\"label\":\"Sleep\",\"action\":\"suspend\"},"
    "{\"icon\":\"power\",\"label\":\"Off\",\"action\":\"shutdown now\"}"
    "]";

static const char *const s_dither_names[BRUCE_DISPLAY_DITHER_COUNT] = {
    "Threshold", "Bayer 2", "Bayer 4", "Noise", "Blue", "Atkinson"};

typedef struct {
    char icon[SYSTEM_MENU__TEXT_MAX];
    char label[SYSTEM_MENU__TEXT_MAX];
    char action[SYSTEM_MENU__ACTION_MAX];
    bool bar;
    int value; /* A bar's 0..100. */
    int row;
    int x;
    int w;
} system_menu__item_t;

static void system_menu__copy_json_string(cJSON *object, const char *key, char *out, size_t capacity) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    const char *text = cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : "";
    snprintf(out, capacity, "%s", text);
}

static size_t system_menu__load_items(system_menu__item_t *items, size_t capacity) {
    const char *defaults =
        display__is_epaper() ? SYSTEM_MENU__DEFAULT_EPAPER_ITEMS_JSON : SYSTEM_MENU__DEFAULT_ITEMS_JSON;
    char *json = memory__malloc(SYSTEM_MENU__CONFIG_MAX);
    if (json == NULL) return 0;
    bool configured =
        app_config__get_json(SYSTEM_MENU__APP_NAME, "items", defaults, json, SYSTEM_MENU__CONFIG_MAX);
    if (!configured) (void)app_config__set_json(SYSTEM_MENU__APP_NAME, "items", defaults);

    cJSON *root = cJSON_Parse(json);
    memory__free(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        root = cJSON_Parse(defaults);
    }

    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (count >= capacity) break;
        if (!cJSON_IsObject(item)) continue;
        system_menu__item_t *entry = &items[count];
        system_menu__copy_json_string(item, "icon", entry->icon, sizeof(entry->icon));
        system_menu__copy_json_string(item, "label", entry->label, sizeof(entry->label));
        system_menu__copy_json_string(item, "action", entry->action, sizeof(entry->action));
        if (entry->action[0] == '\0') continue;
        if (strcmp(entry->action, "volume") == 0) {
            entry->bar = true;
            entry->value = config__get_sound_volume();
        } else if (strcmp(entry->action, "brightness") == 0) {
            entry->bar = true;
            entry->value = config__get_display_brightness();
        } else if (entry->label[0] == '\0') {
            continue;
        }
        ++count;
    }
    cJSON_Delete(root);
    return count;
}

/* Gives every item its row and cell; returns the number of rows. */
static int system_menu__layout(system_menu__item_t *items, size_t count, int width) {
    int rows = 0;
    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        if (!items[i].bar) {
            while (end < count && !items[end].bar) ++end;
        }
        int cells = (int)(end - i);
        for (size_t j = i; j < end; ++j) {
            int cell = (int)(j - i);
            items[j].row = rows;
            items[j].x = cell * width / cells;
            items[j].w = (cell + 1) * width / cells - items[j].x;
        }
        ++rows;
        i = end;
    }
    return rows;
}

static void system_menu__draw_bar(const system_menu__item_t *item, bool active) {
    int y = item->row * SYSTEM_MENU__ROW_H;
    uint16_t primary = config__get_color_primary();
    uint16_t surface = config__get_color_surface();
    uint16_t text = config__get_color_text();
    if (active) (void)display__draw_rect(1, y + 1, item->w - 2, SYSTEM_MENU__ROW_H - 2, primary);

    const bruce_icon_t *icon = icon__get(item->icon);
    if (icon != NULL) {
        (void)display__draw_bitmap_scaled(
            8, y + (SYSTEM_MENU__ROW_H - SYSTEM_MENU__ICON_SIZE) / 2, icon->bits, icon->width, icon->height,
            SYSTEM_MENU__ICON_SIZE, SYSTEM_MENU__ICON_SIZE, text
        );
    }
    int bar_w = item->w - SYSTEM_MENU__BAR_X - SYSTEM_MENU__BAR_RIGHT;
    int bar_y = y + SYSTEM_MENU__ROW_H / 2 - 5;
    (void)display__draw_rect(SYSTEM_MENU__BAR_X, bar_y, bar_w, 10, text);
    (void)display__fill_rect(SYSTEM_MENU__BAR_X + 2, bar_y + 2, (bar_w - 4) * item->value / 100, 6, primary);

    char label[8];
    snprintf(label, sizeof(label), "%d%%", item->value);
    (void)display__set_text_color(text);
    (void)display__set_text_bg_color(surface);
    (void)display__set_text_size(1);
    int label_y = y + SYSTEM_MENU__ROW_H / 2 - 4;
    (void)display__draw_centre_string(label, item->w - SYSTEM_MENU__BAR_RIGHT / 2, label_y);
}

static void system_menu__draw_button(const system_menu__item_t *item, bool active) {
    int y = item->row * SYSTEM_MENU__ROW_H;
    uint16_t primary = config__get_color_primary();
    uint16_t background = config__get_color_background();
    uint16_t surface = config__get_color_surface();
    uint16_t text = config__get_color_text();
    uint16_t fg = active ? background : text;
    uint16_t bg = active ? primary : surface;
    if (active) (void)display__fill_rect(item->x + 1, y + 2, item->w - 2, SYSTEM_MENU__ROW_H - 4, bg);

    const char *label = item->label;
    if (strcmp(item->action, "display.dither") == 0) {
        int dither = config__get_display_dither();
        if (dither >= 0 && dither < BRUCE_DISPLAY_DITHER_COUNT) label = s_dither_names[dither];
    }
    const bruce_icon_t *icon = icon__get(item->icon);
    if (icon != NULL) {
        (void)display__draw_bitmap_scaled(
            item->x + (item->w - SYSTEM_MENU__ICON_SIZE) / 2, y + 4, icon->bits, icon->width, icon->height,
            SYSTEM_MENU__ICON_SIZE, SYSTEM_MENU__ICON_SIZE, fg
        );
    } else {
        char fallback[2] = {label[0], '\0'};
        (void)display__set_text_color(fg);
        (void)display__set_text_bg_color(bg);
        (void)display__set_text_size(2);
        (void)display__draw_centre_string(fallback, item->x + item->w / 2, y + 6);
    }
    (void)display__set_text_color(fg);
    (void)display__set_text_bg_color(bg);
    (void)display__set_text_size(1);
    (void)display__draw_centre_string(label, item->x + item->w / 2, y + SYSTEM_MENU__ROW_H - 13);
}

static bruce_result_t system_menu__draw(
    bruce_display_overlay_id_t overlay, const system_menu__item_t *items, size_t count, int rows,
    int selected, const char *status
) {
    bruce_result_t result = display__overlay_begin(overlay);
    if (result != BRUCE_OK) return result;

    int width = display__width();
    int height = rows * SYSTEM_MENU__ROW_H;
    uint16_t primary = config__get_color_primary();
    uint16_t background = config__get_color_background();
    (void)display__fill_screen(config__get_color_surface());
    (void)display__draw_line(0, height - 1, width - 1, height - 1, primary);

    for (size_t i = 0; i < count; ++i) {
        if (items[i].bar) system_menu__draw_bar(&items[i], (int)i == selected);
        else system_menu__draw_button(&items[i], (int)i == selected);
    }

    if (status != NULL && status[0] != '\0') {
        (void)display__set_text_color(primary);
        (void)display__set_text_bg_color(background);
        (void)display__set_text_size(1);
        (void)display__draw_centre_string(status, width / 2, height / 2);
    }
    return display__overlay_end(overlay);
}

static void system_menu__set_bar(system_menu__item_t *item, int value, bool save) {
    item->value = value < 0 ? 0 : value > 100 ? 100 : value;
    if (strcmp(item->action, "volume") == 0) {
        if (save) (void)config__set_sound_volume(item->value);
    } else {
        (void)display__set_brightness((uint8_t)(item->value * 255 / 100));
        if (save) (void)config__set_display_brightness(item->value);
    }
}

static int system_menu__bar_value(const system_menu__item_t *item, int x) {
    int bar_w = item->w - SYSTEM_MENU__BAR_X - SYSTEM_MENU__BAR_RIGHT;
    return (x - SYSTEM_MENU__BAR_X) * 100 / (bar_w > 0 ? bar_w : 1);
}

static int system_menu__run_action(const char *action) {
    if (strcmp(action, "cancel") == 0) return BRUCE_ERR_CANCELLED;
    if (strcmp(action, "input.esc") == 0) {
        bruce_result_t result = process__to_background();
        if (result != BRUCE_OK) return result;
        const bruce_input_event_t event = {
            .type = BRUCE_INPUT_KEY,
            .action = BRUCE_INPUT_PRESS,
            .code = BRUCE_INPUT_CODE_BACK,
            .value = -1,
        };
        return input__inject(&event);
    }
    if (strcmp(action, "input.enter") == 0) {
        bruce_result_t result = process__to_background();
        if (result != BRUCE_OK) return result;
        const bruce_input_event_t event = {
            .type = BRUCE_INPUT_KEY,
            .action = BRUCE_INPUT_PRESS,
            .code = BRUCE_INPUT_CODE_SELECT,
            .value = BRUCE_INPUT_CODE_SELECT,
        };
        return input__inject(&event);
    }
    if (strcmp(action, "process.next") == 0 || strcmp(action, "process.previous") == 0) {
        /* The menu is a temporary foreground process. Remove it before choosing
         * a neighbor so the process that opened the menu becomes the anchor,
         * rather than selecting according to the menu's recycled slot. */
        bruce_result_t result = process__to_background();
        if (result != BRUCE_OK) return result;
        result = strcmp(action, "process.next") == 0 ? process__switch_next() : process__switch_previous();
        return result == BRUCE_ERR_NOT_FOUND ? BRUCE_OK : result;
    }
    if (strcmp(action, "process.preview") == 0)
        return app_runner__run_command("GUI=1 process preview", BRUCE_LAUNCH_FOREGROUND);
    if (strcmp(action, "launcher") == 0)
        return app_runner__run_command("GUI=1 launcher", BRUCE_LAUNCH_FOREGROUND);
    if (strcmp(action, "display.rotate") == 0) {
        int turns = (config__get_display_rotation() & 3) == 0 ? 1 : 0;
        bruce_result_t result = config__set_display_rotation(turns);
        return result == BRUCE_OK ? display__set_rotation((uint8_t)turns) : result;
    }
    /* These keep the picture an e-paper panel shows; let it drop the menu first. */
    if (strcmp(action, "suspend") == 0 || strncmp(action, "shutdown", 8) == 0) {
        (void)display__wait_shown(3000);
    }
    if (strcmp(action, "device.power_off") == 0) {
        (void)display__wait_shown(3000);
        return device__power_off(0);
    }
    return app_runner__run_command(action, BRUCE_LAUNCH_FOREGROUND);
}

int system_menu_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        stdio__printf("Open the system menu.\n");
        return BRUCE_OK;
    }

    system_menu__item_t *items = memory__calloc(SYSTEM_MENU__MAX_ITEMS, sizeof(*items));
    if (items == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t count = system_menu__load_items(items, SYSTEM_MENU__MAX_ITEMS);
    if (count == 0) {
        memory__free(items);
        return BRUCE_ERR_NOT_FOUND;
    }
    int rows = system_menu__layout(items, count, display__width());
    int height = rows * SYSTEM_MENU__ROW_H;

    bruce_display_overlay_id_t overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
    bruce_result_t overlay_result = display__overlay_create(0, 0, display__width(), height, &overlay);
    if (overlay_result != BRUCE_OK) {
        memory__free(items);
        return overlay_result;
    }

    /* On touch screens nothing is highlighted until a key is pressed. */
    int selected = input__has_touch() ? -1 : 0;
    int dragging = -1; /* The bar a finger is on. */
    int run = -1;      /* The item to run next. */
    char status[48] = {0};
    bool redraw = true;
    bool overlay_shown = false;
    bruce_result_t result = BRUCE_OK;
    while (process__current_signal() == 0) {
        if (redraw) {
            bruce_result_t draw = system_menu__draw(overlay, items, count, rows, selected, status);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            if (draw != BRUCE_OK) {
                result = draw;
                break;
            }
            if (!overlay_shown) {
                result = display__overlay_show(overlay);
                if (result != BRUCE_OK) break;
                overlay_shown = true;
            }
            redraw = false;
        }

        bruce_input_event_t event;
        bruce_result_t input = input__read(&event, 100);
        if (input == BRUCE_ERR_NOT_FOREGROUND) break;
        if (input != BRUCE_OK) continue;

        if (event.code == BRUCE_INPUT_CODE_TOUCH) {
            int row = event.y / SYSTEM_MENU__ROW_H;
            int hit = -1;
            for (size_t i = 0; i < count; ++i) {
                bool inside = event.x >= items[i].x && event.x < items[i].x + items[i].w;
                if (items[i].row == row && inside) hit = (int)i;
            }
            if (event.action == BRUCE_INPUT_PRESS) {
                if (event.y >= height) break; /* A tap outside the menu closes it. */
                dragging = hit >= 0 && items[hit].bar ? hit : -1;
            }
            if (dragging >= 0) {
                bool release = event.action == BRUCE_INPUT_RELEASE;
                int value = system_menu__bar_value(&items[dragging], event.x);
                system_menu__set_bar(&items[dragging], value, release);
                if (release) dragging = -1;
                redraw = true;
            } else if (event.action == BRUCE_INPUT_RELEASE && hit >= 0 && event.y < height) {
                run = hit;
            }
        } else if (event.action == BRUCE_INPUT_PRESS) {
            status[0] = '\0';
            bool on_bar = selected >= 0 && items[selected].bar;
            if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_MENU) break;
            if (on_bar && (event.code == BRUCE_INPUT_CODE_LEFT || event.code == BRUCE_INPUT_CODE_RIGHT)) {
                int step = SYSTEM_MENU__BAR_STEP;
                if (event.code == BRUCE_INPUT_CODE_LEFT) step = -step;
                system_menu__set_bar(&items[selected], items[selected].value + step, true);
            } else if (event.code == BRUCE_INPUT_CODE_LEFT || event.code == BRUCE_INPUT_CODE_UP ||
                       event.code == BRUCE_INPUT_CODE_PREV) {
                selected = selected <= 0 ? (int)count - 1 : selected - 1;
            } else if (event.code == BRUCE_INPUT_CODE_RIGHT || event.code == BRUCE_INPUT_CODE_DOWN ||
                       event.code == BRUCE_INPUT_CODE_NEXT) {
                selected = (selected + 1) % (int)count;
            } else if (event.code == BRUCE_INPUT_CODE_SELECT && selected >= 0 && !items[selected].bar) {
                run = selected;
            }
            redraw = true;
        }
        if (run < 0) continue;

        const char *action = items[run].action;
        run = -1;
        redraw = true;
        if (strcmp(action, "display.dither") == 0) {
            /* Stays open, so the effect can be watched. */
            (void)config__set_display_dither((config__get_display_dither() + 1) % BRUCE_DISPLAY_DITHER_COUNT);
            continue;
        }
        /* Actions may open a full-screen dialog. The menu's overlay sits
         * above that dialog until it is hidden explicitly. */
        if (overlay_shown) {
            bruce_result_t hide_result = display__overlay_hide(overlay);
            if (hide_result != BRUCE_OK) {
                snprintf(status, sizeof(status), "%s", result__to_string(hide_result));
                continue;
            }
            overlay_shown = false;
        }
        int action_result = system_menu__run_action(action);
        if (action_result == BRUCE_OK || action_result == BRUCE_ERR_CANCELLED) break;
        snprintf(status, sizeof(status), "%s", result__to_string(action_result));
    }
    if (dragging >= 0) system_menu__set_bar(&items[dragging], items[dragging].value, true);
    (void)display__overlay_destroy(overlay);
    memory__free(items);
    return result;
}
