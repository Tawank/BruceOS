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
#define SYSTEM_MENU__BAR_H 42
#define SYSTEM_MENU__ICON_SIZE 20

static const char *const SYSTEM_MENU__DEFAULT_ITEMS_JSON =
    "["
    "{\"icon\":\"cancel\",\"label\":\"Cancel\",\"action\":\"cancel\"},"
    "{\"icon\":\"keyboard-esc\",\"label\":\"Esc\",\"action\":\"input.esc\"},"
    "{\"icon\":\"keyboard-return\",\"label\":\"Enter\",\"action\":\"input.enter\"},"
    "{\"icon\":\"arrange-bring-forward\",\"label\":\"Switch\",\"action\":\"process.preview\"},"
    "{\"icon\":\"apps\",\"label\":\"Launcher\",\"action\":\"launcher\"},"
    "{\"icon\":\"power\",\"label\":\"Off\",\"action\":\"shutdown now\"}"
    "]";

typedef struct {
    char icon[SYSTEM_MENU__TEXT_MAX];
    char label[SYSTEM_MENU__TEXT_MAX];
    char action[SYSTEM_MENU__ACTION_MAX];
} system_menu__item_t;

static void system_menu__copy_json_string(cJSON *object, const char *key, char *out, size_t capacity) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    const char *text = cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : "";
    snprintf(out, capacity, "%s", text);
}

static size_t system_menu__load_items(system_menu__item_t *items, size_t capacity) {
    char *json = memory__malloc(SYSTEM_MENU__CONFIG_MAX);
    if (json == NULL) return 0;
    bool configured = app_config__get_json(
        SYSTEM_MENU__APP_NAME, "items", SYSTEM_MENU__DEFAULT_ITEMS_JSON, json, SYSTEM_MENU__CONFIG_MAX
    );
    if (!configured) {
        (void)app_config__set_json(SYSTEM_MENU__APP_NAME, "items", SYSTEM_MENU__DEFAULT_ITEMS_JSON);
    }

    cJSON *root = cJSON_Parse(json);
    memory__free(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        root = cJSON_Parse(SYSTEM_MENU__DEFAULT_ITEMS_JSON);
    }

    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (count >= capacity) break;
        if (!cJSON_IsObject(item)) continue;
        system_menu__copy_json_string(item, "icon", items[count].icon, sizeof(items[count].icon));
        system_menu__copy_json_string(item, "label", items[count].label, sizeof(items[count].label));
        system_menu__copy_json_string(item, "action", items[count].action, sizeof(items[count].action));
        if (items[count].label[0] == '\0' || items[count].action[0] == '\0') continue;
        ++count;
    }
    cJSON_Delete(root);
    return count;
}

static bruce_result_t system_menu__draw(
    bruce_display_overlay_id_t overlay, const system_menu__item_t *items, size_t count, int selected,
    const char *status
) {
    bruce_result_t result = display__overlay_begin(overlay);
    if (result != BRUCE_OK) return result;

    int width = display__width();
    uint16_t primary = config__get_color_primary();
    uint16_t background = config__get_color_background();
    uint16_t surface = config__get_color_surface();
    uint16_t text = config__get_color_text();

    (void)display__fill_screen(surface);
    (void)display__draw_line(0, SYSTEM_MENU__BAR_H - 1, width - 1, SYSTEM_MENU__BAR_H - 1, primary);

    int cell_w = count > 0 ? width / (int)count : width;
    if (cell_w < 1) cell_w = 1;
    for (size_t i = 0; i < count; ++i) {
        int x = (int)i * cell_w;
        int w = i == count - 1 ? width - x : cell_w;
        bool active = (int)i == selected;
        uint16_t fg = active ? background : text;
        uint16_t bg = active ? primary : surface;
        if (active) (void)display__fill_rect(x + 1, 2, w - 2, SYSTEM_MENU__BAR_H - 4, bg);

        const bruce_icon_t *icon = icon__get(items[i].icon);
        if (icon != NULL) {
            int icon_x = x + (w - SYSTEM_MENU__ICON_SIZE) / 2;
            (void)display__draw_bitmap_scaled(
                icon_x,
                4,
                icon->bits,
                icon->width,
                icon->height,
                SYSTEM_MENU__ICON_SIZE,
                SYSTEM_MENU__ICON_SIZE,
                fg
            );
        } else {
            char fallback[2] = {items[i].label[0], '\0'};
            (void)display__set_text_color(fg);
            (void)display__set_text_bg_color(bg);
            (void)display__set_text_size(2);
            (void)display__draw_centre_string(fallback, x + w / 2, 6);
        }

        (void)display__set_text_color(fg);
        (void)display__set_text_bg_color(bg);
        (void)display__set_text_size(1);
        (void)display__draw_centre_string(items[i].label, x + w / 2, SYSTEM_MENU__BAR_H - 13);
    }

    if (status != NULL && status[0] != '\0') {
        (void)display__set_text_color(primary);
        (void)display__set_text_bg_color(background);
        (void)display__set_text_size(1);
        (void)display__draw_centre_string(status, width / 2, SYSTEM_MENU__BAR_H / 2);
    }
    return display__overlay_end(overlay);
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
    if (strcmp(action, "device.power_off") == 0) return device__power_off(0);
    else { return app_runner__run_command(action, BRUCE_LAUNCH_FOREGROUND); }
    return BRUCE_ERR_INVALID_ARGUMENT;
}

/* --- Touch layout: volume and brightness bars, then a row of buttons. --- */

#define SYSTEM_MENU__ROW_H 40
#define SYSTEM_MENU__BAR_X 34
#define SYSTEM_MENU__BAR_RIGHT 40
#define SYSTEM_MENU__BAR_STEP 10

enum {
    SYSTEM_MENU__VOLUME,
    SYSTEM_MENU__BRIGHTNESS,
    SYSTEM_MENU__DITHER,
    SYSTEM_MENU__ROTATE,
    SYSTEM_MENU__SLEEP,
    SYSTEM_MENU__SHUTDOWN,
    SYSTEM_MENU__TOUCH_ITEMS
};

static const char *const s_button_icons[4] = {"dither", "rotate", "sleep", "power"};
static const char *const s_button_labels[4] = {"Dither", "Rotate", "Sleep", "Off"};
static const char *const s_dither_names[BRUCE_DISPLAY_DITHER_COUNT] = {
    "Threshold", "Bayer 2", "Bayer 4", "Noise", "Blue"};

typedef struct {
    int volume;     /* 0..100 */
    int brightness; /* 0..100 */
    int selected;   /* Keyboard focus, SYSTEM_MENU__* */
} system_menu__touch_t;

static void system_menu__draw_bar(int row, const char *icon_name, int value, bool focused) {
    int width = display__width();
    int y = row * SYSTEM_MENU__ROW_H;
    uint16_t primary = config__get_color_primary();
    uint16_t text = config__get_color_text();
    uint16_t surface = config__get_color_surface();
    if (focused) (void)display__draw_rect(1, y + 1, width - 2, SYSTEM_MENU__ROW_H - 2, primary);

    const bruce_icon_t *icon = icon__get(icon_name);
    if (icon != NULL) {
        (void)display__draw_bitmap_scaled(
            8, y + (SYSTEM_MENU__ROW_H - SYSTEM_MENU__ICON_SIZE) / 2, icon->bits, icon->width, icon->height,
            SYSTEM_MENU__ICON_SIZE, SYSTEM_MENU__ICON_SIZE, text
        );
    }
    int bar_w = width - SYSTEM_MENU__BAR_X - SYSTEM_MENU__BAR_RIGHT;
    int bar_y = y + SYSTEM_MENU__ROW_H / 2 - 5;
    (void)display__draw_rect(SYSTEM_MENU__BAR_X, bar_y, bar_w, 10, text);
    (void)display__fill_rect(SYSTEM_MENU__BAR_X + 2, bar_y + 2, (bar_w - 4) * value / 100, 6, primary);

    char label[8];
    snprintf(label, sizeof(label), "%d%%", value);
    (void)display__set_text_color(text);
    (void)display__set_text_bg_color(surface);
    (void)display__set_text_size(1);
    int label_y = y + SYSTEM_MENU__ROW_H / 2 - 4;
    (void)display__draw_centre_string(label, width - SYSTEM_MENU__BAR_RIGHT / 2, label_y);
}

static bruce_result_t
system_menu__touch_draw(bruce_display_overlay_id_t overlay, const system_menu__touch_t *menu) {
    bruce_result_t result = display__overlay_begin(overlay);
    if (result != BRUCE_OK) return result;

    int width = display__width();
    int height = 3 * SYSTEM_MENU__ROW_H;
    uint16_t primary = config__get_color_primary();
    uint16_t background = config__get_color_background();
    uint16_t surface = config__get_color_surface();
    uint16_t text = config__get_color_text();
    (void)display__fill_screen(surface);
    (void)display__draw_line(0, height - 1, width - 1, height - 1, primary);

    system_menu__draw_bar(0, "volume-high", menu->volume, menu->selected == SYSTEM_MENU__VOLUME);
    system_menu__draw_bar(1, "brightness", menu->brightness, menu->selected == SYSTEM_MENU__BRIGHTNESS);

    int y = 2 * SYSTEM_MENU__ROW_H;
    int cell_w = width / 4;
    int dither = config__get_display_dither();
    for (int i = 0; i < 4; ++i) {
        int x = i * cell_w;
        int w = i == 3 ? width - x : cell_w;
        bool active = menu->selected == SYSTEM_MENU__DITHER + i;
        uint16_t fg = active ? background : text;
        uint16_t bg = active ? primary : surface;
        if (active) (void)display__fill_rect(x + 1, y + 2, w - 2, SYSTEM_MENU__ROW_H - 4, bg);
        const bruce_icon_t *icon = icon__get(s_button_icons[i]);
        if (icon != NULL) {
            (void)display__draw_bitmap_scaled(
                x + (w - SYSTEM_MENU__ICON_SIZE) / 2, y + 4, icon->bits, icon->width, icon->height,
                SYSTEM_MENU__ICON_SIZE, SYSTEM_MENU__ICON_SIZE, fg
            );
        }
        const char *label = s_button_labels[i];
        if (i == 0 && dither >= 0 && dither < BRUCE_DISPLAY_DITHER_COUNT) label = s_dither_names[dither];
        (void)display__set_text_color(fg);
        (void)display__set_text_bg_color(bg);
        (void)display__set_text_size(1);
        (void)display__draw_centre_string(label, x + w / 2, y + SYSTEM_MENU__ROW_H - 13);
    }
    return display__overlay_end(overlay);
}

static int system_menu__bar_value(int x) {
    int bar_w = display__width() - SYSTEM_MENU__BAR_X - SYSTEM_MENU__BAR_RIGHT;
    int value = (x - SYSTEM_MENU__BAR_X) * 100 / (bar_w > 0 ? bar_w : 1);
    return value < 0 ? 0 : value > 100 ? 100 : value;
}

static void system_menu__set_bar(system_menu__touch_t *menu, int item, int value, bool save) {
    value = value < 0 ? 0 : value > 100 ? 100 : value;
    if (item == SYSTEM_MENU__VOLUME) {
        menu->volume = value;
        if (save) (void)config__set_sound_volume(value);
    } else {
        menu->brightness = value;
        (void)display__set_brightness((uint8_t)(value * 255 / 100));
        if (save) (void)config__set_display_brightness(value);
    }
}

/* Runs a button; true when the menu should close. */
static bool system_menu__press_button(bruce_display_overlay_id_t overlay, bool *shown, int item) {
    if (item == SYSTEM_MENU__DITHER) {
        (void)config__set_display_dither((config__get_display_dither() + 1) % BRUCE_DISPLAY_DITHER_COUNT);
        return false;
    }
    if (*shown && display__overlay_hide(overlay) == BRUCE_OK) *shown = false;
    if (item == SYSTEM_MENU__ROTATE) {
        int turns = (config__get_display_rotation() & 3) == 0 ? 1 : 0;
        if (config__set_display_rotation(turns) == BRUCE_OK) (void)display__set_rotation((uint8_t)turns);
    } else if (item == SYSTEM_MENU__SLEEP) {
        uint8_t brightness = display__get_brightness();
        (void)display__set_brightness(0);
        (void)runtime__delay(1500); /* Let an e-paper panel finish drawing the screen without the menu. */
        (void)device__sleep();
        (void)display__set_brightness(brightness);
    } else if (item == SYSTEM_MENU__SHUTDOWN) {
        (void)system_menu__run_action("shutdown now");
    }
    return true;
}

static int system_menu__touch_main(void) {
    int height = 3 * SYSTEM_MENU__ROW_H;
    bruce_display_overlay_id_t overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
    bruce_result_t result = display__overlay_create(0, 0, display__width(), height, &overlay);
    if (result != BRUCE_OK) return result;

    system_menu__touch_t menu = {
        .volume = config__get_sound_volume(),
        .brightness = config__get_display_brightness(),
        .selected = -1,
    };
    int dragging = -1; /* The bar a finger is on. */
    bool redraw = true;
    bool shown = false;
    bool done = false;
    while (!done && process__current_signal() == 0) {
        if (redraw) {
            bruce_result_t draw = system_menu__touch_draw(overlay, &menu);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            if (draw != BRUCE_OK) {
                result = draw;
                break;
            }
            if (!shown) {
                result = display__overlay_show(overlay);
                if (result != BRUCE_OK) break;
                shown = true;
            }
            redraw = false;
        }

        bruce_input_event_t event;
        bruce_result_t input = input__read(&event, 100);
        if (input == BRUCE_ERR_NOT_FOREGROUND) break;
        if (input != BRUCE_OK) continue;

        if (event.code == BRUCE_INPUT_CODE_TOUCH) {
            int row = event.y / SYSTEM_MENU__ROW_H;
            if (event.action == BRUCE_INPUT_PRESS) {
                if (event.y >= height) break; /* A tap outside the menu closes it. */
                dragging = row < 2 ? row : -1;
            }
            if (dragging >= 0) {
                bool release = event.action == BRUCE_INPUT_RELEASE;
                system_menu__set_bar(&menu, dragging, system_menu__bar_value(event.x), release);
                if (release) dragging = -1;
                redraw = true;
            } else if (event.action == BRUCE_INPUT_RELEASE && row == 2) {
                int column = event.x * 4 / display__width();
                done = system_menu__press_button(overlay, &shown, SYSTEM_MENU__DITHER + column);
                redraw = !done;
            }
            continue;
        }

        if (event.action != BRUCE_INPUT_PRESS) continue;
        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_MENU) break;
        int item = menu.selected;
        if (event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_PREV) {
            menu.selected = item <= 0 ? SYSTEM_MENU__TOUCH_ITEMS - 1 : item - 1;
        } else if (event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT) {
            menu.selected = (item + 1) % SYSTEM_MENU__TOUCH_ITEMS;
        } else if (item >= 0 &&
                   (event.code == BRUCE_INPUT_CODE_LEFT || event.code == BRUCE_INPUT_CODE_RIGHT)) {
            int step = event.code == BRUCE_INPUT_CODE_LEFT ? -1 : 1;
            if (item <= SYSTEM_MENU__BRIGHTNESS) {
                int value = item == SYSTEM_MENU__VOLUME ? menu.volume : menu.brightness;
                system_menu__set_bar(&menu, item, value + step * SYSTEM_MENU__BAR_STEP, true);
            } else {
                menu.selected = SYSTEM_MENU__DITHER + (item - SYSTEM_MENU__DITHER + step + 4) % 4;
            }
        } else if (event.code == BRUCE_INPUT_CODE_SELECT && item >= SYSTEM_MENU__DITHER) {
            done = system_menu__press_button(overlay, &shown, item);
        } else {
            continue;
        }
        redraw = !done;
    }
    if (dragging >= 0) {
        system_menu__set_bar(&menu, dragging, dragging == 0 ? menu.volume : menu.brightness, true);
    }
    (void)display__overlay_destroy(overlay);
    return result;
}

int system_menu_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        stdio__printf("Open the system menu.\n");
        return BRUCE_OK;
    }
    if (input__has_touch()) return system_menu__touch_main();

    system_menu__item_t *items = memory__calloc(SYSTEM_MENU__MAX_ITEMS, sizeof(*items));
    if (items == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t count = system_menu__load_items(items, SYSTEM_MENU__MAX_ITEMS);
    if (count == 0) {
        memory__free(items);
        return BRUCE_ERR_NOT_FOUND;
    }

    bruce_display_overlay_id_t overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
    bruce_result_t overlay_result =
        display__overlay_create(0, 0, display__width(), SYSTEM_MENU__BAR_H, &overlay);
    if (overlay_result != BRUCE_OK) {
        memory__free(items);
        return overlay_result;
    }

    int selected = 0;
    char status[48] = {0};
    bool redraw = true;
    bool overlay_shown = false;
    bruce_result_t result = BRUCE_OK;
    while (process__current_signal() == 0) {
        if (redraw) {
            bruce_result_t draw = system_menu__draw(overlay, items, count, selected, status);
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
        if (input != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) continue;
        status[0] = '\0';

        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_MENU) break;
        if (event.code == BRUCE_INPUT_CODE_LEFT || event.code == BRUCE_INPUT_CODE_UP ||
            event.code == BRUCE_INPUT_CODE_PREV) {
            selected = (selected + (int)count - 1) % (int)count;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_RIGHT || event.code == BRUCE_INPUT_CODE_DOWN ||
                   event.code == BRUCE_INPUT_CODE_NEXT) {
            selected = (selected + 1) % (int)count;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_SELECT) {
            /* Actions may open a full-screen dialog. The menu's overlay sits
             * above that dialog until it is hidden explicitly. */
            if (overlay_shown) {
                bruce_result_t hide_result = display__overlay_hide(overlay);
                if (hide_result != BRUCE_OK) {
                    snprintf(status, sizeof(status), "%s", result__to_string(hide_result));
                    redraw = true;
                    continue;
                }
                overlay_shown = false;
            }
            int action_result = system_menu__run_action(items[selected].action);
            if (action_result == BRUCE_OK || action_result == BRUCE_ERR_CANCELLED) break;
            snprintf(status, sizeof(status), "%s", result__to_string(action_result));
            redraw = true;
        }
    }
    (void)display__overlay_destroy(overlay);
    memory__free(items);
    return result;
}
