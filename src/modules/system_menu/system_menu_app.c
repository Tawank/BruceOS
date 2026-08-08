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
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"

#define SYSTEM_MENU__APP_NAME "menu"
#define SYSTEM_MENU__MAX_ITEMS 8
#define SYSTEM_MENU__TEXT_MAX 32
#define SYSTEM_MENU__ACTION_MAX 64
#define SYSTEM_MENU__CONFIG_MAX 1024
#define SYSTEM_MENU__BAR_H 42
#define SYSTEM_MENU__ICON_SIZE 20

static const char *const SYSTEM_MENU__DEFAULT_ITEMS_JSON =
    "["
    "{\"icon\":\"close\",\"label\":\"Cancel\",\"action\":\"cancel\"},"
    "{\"icon\":\"swap-horizontal\",\"label\":\"Next\",\"action\":\"process.next\"},"
    "{\"icon\":\"apps\",\"label\":\"Launcher\",\"action\":\"launcher\"},"
    "{\"icon\":\"power\",\"label\":\"Off\",\"action\":\"device.power_off\"}"
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
    char json[SYSTEM_MENU__CONFIG_MAX];
    (void)app_config__get_json(
        SYSTEM_MENU__APP_NAME, "items", SYSTEM_MENU__DEFAULT_ITEMS_JSON, json, sizeof(json)
    );

    cJSON *root = cJSON_Parse(json);
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

static bruce_result_t system_menu__draw(const system_menu__item_t *items, size_t count, int selected, const char *status) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;

    int width = display__width();
    int height = display__height();
    uint16_t primary = config__get_pri_color();
    uint16_t secondary = config__get_sec_color();
    uint16_t background = config__get_bg_color();

    (void)display__fill_screen(background);
    (void)display__fill_rect(0, 0, width, SYSTEM_MENU__BAR_H, secondary);
    (void)display__draw_line(0, SYSTEM_MENU__BAR_H - 1, width - 1, SYSTEM_MENU__BAR_H - 1, primary);

    int cell_w = count > 0 ? width / (int)count : width;
    if (cell_w < 1) cell_w = 1;
    for (size_t i = 0; i < count; ++i) {
        int x = (int)i * cell_w;
        int w = i == count - 1 ? width - x : cell_w;
        bool active = (int)i == selected;
        uint16_t fg = active ? background : primary;
        uint16_t bg = active ? primary : secondary;
        if (active) (void)display__fill_rect(x + 1, 2, w - 2, SYSTEM_MENU__BAR_H - 4, bg);

        const bruce_icon_t *icon = icon__get(items[i].icon);
        if (icon != NULL) {
            int icon_x = x + (w - SYSTEM_MENU__ICON_SIZE) / 2;
            (void)display__draw_bitmap_scaled(
                icon_x, 4, icon->bits, icon->width, icon->height, SYSTEM_MENU__ICON_SIZE,
                SYSTEM_MENU__ICON_SIZE, fg
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
        (void)display__draw_centre_string(items[i].label, x + w / 2, 29);
    }

    if (status != NULL && status[0] != '\0') {
        (void)display__set_text_color(primary);
        (void)display__set_text_bg_color(background);
        (void)display__set_text_size(1);
        (void)display__draw_centre_string(status, width / 2, height / 2);
    }
    return display__present();
}

static int system_menu__run_action(const char *action) {
    if (strcmp(action, "cancel") == 0) return BRUCE_ERR_CANCELLED;
    if (strcmp(action, "process.next") == 0) return process__switch_next();
    if (strcmp(action, "process.previous") == 0) return process__switch_previous();
    if (strcmp(action, "launcher") == 0) return app_runner__run_command("GUI=1 launcher", BRUCE_LAUNCH_FOREGROUND);
    if (strcmp(action, "device.power_off") == 0) return device__power_off();
    if (strncmp(action, "command:", 8) == 0) {
        const char *command = action + 8;
        while (*command == ' ') command++;
        return app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);
    }
    return BRUCE_ERR_INVALID_ARGUMENT;
}

int system_menu_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    system_menu__item_t items[SYSTEM_MENU__MAX_ITEMS] = {0};
    size_t count = system_menu__load_items(items, SYSTEM_MENU__MAX_ITEMS);
    if (count == 0) return BRUCE_ERR_NOT_FOUND;

    int selected = 0;
    char status[48] = {0};
    bool redraw = true;
    while (process__current_signal() == 0) {
        if (redraw) {
            bruce_result_t draw = system_menu__draw(items, count, selected, status);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            if (draw != BRUCE_OK) return draw;
            redraw = false;
        }

        bruce_input_event_t event;
        bruce_result_t input = input__read(&event, 100);
        if (input == BRUCE_ERR_NOT_FOREGROUND) return BRUCE_OK;
        if (input != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) continue;
        status[0] = '\0';

        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_MENU) return BRUCE_OK;
        if (event.code == BRUCE_INPUT_CODE_LEFT) {
            selected = (selected + (int)count - 1) % (int)count;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_RIGHT) {
            selected = (selected + 1) % (int)count;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_SELECT) {
            int result = system_menu__run_action(items[selected].action);
            if (result == BRUCE_OK || result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
            snprintf(status, sizeof(status), "%s", app_runner__result_to_string(result));
            redraw = true;
        }
    }
    return BRUCE_OK;
}
