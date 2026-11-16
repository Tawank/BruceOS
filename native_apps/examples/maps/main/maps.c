#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "bruce_sdk.h"
#include "geo.h"
#include "hud.h"
#include "map_view.h"
#include "settings.h"
#include "tiles.h"

#define MAPS_IDLE_DELAY_MS 30
#define MAPS_GOTO_ZOOM 15
#define MAPS_LONG_PRESS_MS 500
#define MAPS_HINT_MS 2000

typedef struct {
    settings_t settings;
    map_view_t view;
    map_view_t start_view;
    tiles_t tiles;
    hud_state_t hud;
    uint64_t hint_until_ms;
    bool running;
    bool dirty;
    int32_t touch_x;
    int32_t touch_y;
    bool touch_on_button;
} maps_t;

static int32_t maps__to_e6(double degrees) { return (int32_t)floor(degrees * 1000000.0 + 0.5); }

static void maps__pan(maps_t *app, int dx, int dy) {
    map_view__pan(&app->view, dx, dy);
    app->dirty = true;
}

static void maps__zoom(maps_t *app, int delta) {
    map_view__zoom(&app->view, delta);
    app->dirty = true;
}

static const char *const s_nav_names[HUD_NAV_COUNT] = {"zoom", "up/down", "left/right"};
static const char *const s_nav_hints[HUD_NAV_COUNT] = {"Zoom in/out", "Move up/down", "Move left/right"};

/* Prev/Next on button and encoder boards. */
static void maps__step(maps_t *app, int direction) {
    switch (app->hud.nav_mode) {
        case HUD_NAV_ZOOM: maps__zoom(app, direction); break;
        case HUD_NAV_UP_DOWN: maps__pan(app, 0, direction * display__height() / 4); break;
        case HUD_NAV_LEFT_RIGHT: maps__pan(app, direction * display__width() / 4, 0); break;
        case HUD_NAV_COUNT: break;
    }
}

static hud_nav_mode_t maps__next_nav_mode(const maps_t *app) {
    return (app->hud.nav_mode + 1) % HUD_NAV_COUNT;
}

static void maps__set_nav_mode(maps_t *app, hud_nav_mode_t mode, bool from_menu) {
    app->hud.nav_mode = mode;
    app->hud.hint[0] = s_nav_hints[mode];
    app->hud.hint[1] = from_menu ? "Tip: hold Select" : NULL;
    app->hint_until_ms = runtime__now() + MAPS_HINT_MS;
    app->dirty = true;
}

static void maps__expire_hint(maps_t *app) {
    if (app->hud.hint[0] == NULL || runtime__now() < app->hint_until_ms) return;
    app->hud.hint[0] = app->hud.hint[1] = NULL;
    app->dirty = true;
}

static void maps__go_to(maps_t *app) {
    char text[48] = {0};
    if (dialog__text_input("Go to", "lat, lon", NULL, false, text, sizeof(text)) != BRUCE_OK) return;

    char *end = NULL;
    double lat = strtod(text, &end);
    const char *lon_text = end;
    while (*lon_text == ',' || *lon_text == ' ') lon_text++;
    double lon = strtod(lon_text, &end);
    bool valid = end != lon_text && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
    if (!valid) {
        dialog__message(BRUCE_DIALOG_WARNING, "Go to", "Use the form: 50.0614, 19.9372");
        return;
    }
    int zoom = app->view.zoom > MAPS_GOTO_ZOOM ? app->view.zoom : MAPS_GOTO_ZOOM;
    map_view__set(&app->view, lat, lon, zoom);
}

static void maps__toggle_online(maps_t *app) {
    tiles__set_online(&app->tiles, !app->tiles.online);
    settings__save_online(app->tiles.online);
}

static void maps__menu(maps_t *app) {
    enum { NAV_MODE, GO_TO, ONLINE, EXIT, BACK };
    hud_nav_mode_t next_mode = maps__next_nav_mode(app);
    char nav_label[32];
    snprintf(nav_label, sizeof(nav_label), "Change to %s", s_nav_names[next_mode]);
    const bruce_dialog_choice_t choices[] = {
        [NAV_MODE] = {.label = nav_label, .icon_name = hud__nav_icon(next_mode)},
        [GO_TO] = {.label = "Go to coordinates", .icon_name = "map-marker"},
        [ONLINE] = {.label = "Online tiles", .icon_name = "earth",
                    .right_text = app->tiles.online ? "On" : "Off"},
        [EXIT] = {.label = "Exit", .icon_name = "power"},
        [BACK] = {.label = "Back", .icon_name = "arrow-left"},
    };
    size_t selected = 0;
    if (dialog__choice("Maps", NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected) == BRUCE_OK) {
        switch (selected) {
            case NAV_MODE: maps__set_nav_mode(app, next_mode, true); break;
            case GO_TO: maps__go_to(app); break;
            case ONLINE: maps__toggle_online(app); break;
            case EXIT: app->running = false; break;
            case BACK: break;
        }
    }
    app->dirty = true;
}

/* Hold Select to switch what Prev/Next do; a click opens the menu. */
static void maps__select(maps_t *app, const bruce_input_event_t *event) {
    if (event->type != BRUCE_INPUT_KEY && input__wait_long_press(event, MAPS_LONG_PRESS_MS)) {
        maps__set_nav_mode(app, maps__next_nav_mode(app), false);
        return;
    }
    maps__menu(app);
}

static bool maps__hit(hud_button_t button, int32_t x, int32_t y) {
    hud_rect_t rect = hud__button_rect(button, display__width(), display__height());
    return x >= rect.x && x < rect.x + rect.size && y >= rect.y && y < rect.y + rect.size;
}

/* Drag pans; the +/- buttons appear with the first touch, so keypad boards never see them. */
static void maps__touch(maps_t *app, const bruce_input_event_t *event) {
    if (event->action == BRUCE_INPUT_PRESS) {
        app->dirty |= !app->hud.zoom_buttons;
        app->hud.zoom_buttons = true;
        app->touch_on_button = true;
        if (maps__hit(HUD_BUTTON_ZOOM_IN, event->code, event->value)) maps__zoom(app, 1);
        else if (maps__hit(HUD_BUTTON_ZOOM_OUT, event->code, event->value)) maps__zoom(app, -1);
        else app->touch_on_button = false;
    } else if (event->action == BRUCE_INPUT_CHANGE && !app->touch_on_button) {
        maps__pan(app, app->touch_x - event->code, app->touch_y - event->value);
    }
    app->touch_x = event->code;
    app->touch_y = event->value;
}

static void maps__handle(maps_t *app, const bruce_input_event_t *event) {
    if (event->type == BRUCE_INPUT_TOUCH) {
        maps__touch(app, event);
        return;
    }
    if (event->action != BRUCE_INPUT_PRESS) return;

    int step_x = display__width() / 4;
    int step_y = display__height() / 4;
    switch (event->code) {
        case BRUCE_INPUT_CODE_BACK: app->running = false; break;
        case BRUCE_INPUT_CODE_UP: maps__pan(app, 0, -step_y); break;
        case BRUCE_INPUT_CODE_DOWN: maps__pan(app, 0, step_y); break;
        case BRUCE_INPUT_CODE_LEFT: maps__pan(app, -step_x, 0); break;
        case BRUCE_INPUT_CODE_RIGHT: maps__pan(app, step_x, 0); break;
        case BRUCE_INPUT_CODE_ZOOM_IN:
        case '+':
        case '=': maps__zoom(app, 1); break;
        case BRUCE_INPUT_CODE_ZOOM_OUT:
        case '-': maps__zoom(app, -1); break;
        case BRUCE_INPUT_CODE_NEXT: maps__step(app, 1); break;
        case BRUCE_INPUT_CODE_PREV: maps__step(app, -1); break;
        case BRUCE_INPUT_CODE_MENU: maps__menu(app); break;
        case BRUCE_INPUT_CODE_SELECT: maps__select(app, event); break;
        default: break;
    }
}

static void maps__update_status(maps_t *app, size_t pending) {
    bool offline = pending == 0 && app->tiles.network_down;
    app->hud.status = pending > 0 ? "Loading" : offline ? "No Wi-Fi" : NULL;
    app->hud.status_icon = offline ? "wifi-cancel" : NULL;
}

static bool maps__draw(maps_t *app, int width, int height) {
    if (display__begin_frame() != BRUCE_OK) return false;
    map_view__draw(&app->view, &app->tiles, width, height);
    hud__draw(&app->view, &app->hud, width, height);
    display__present();
    return true;
}

/* Compared in map pixels: the stored lat/lon never round-trips exactly at low zoom. */
static void maps__save_view(maps_t *app) {
    bool moved = app->view.zoom != app->start_view.zoom || app->view.center.x != app->start_view.center.x ||
                 app->view.center.y != app->start_view.center.y;
    if (!moved) return;
    double lat = 0, lon = 0;
    map_view__center(&app->view, &lat, &lon);
    settings_t view = app->settings;
    view.lat_e6 = maps__to_e6(lat);
    view.lon_e6 = maps__to_e6(lon);
    view.zoom = app->view.zoom;
    settings__save_view(&view);
}

static void maps__run(maps_t *app) {
    while (app->running) {
        bruce_input_event_t event;
        while (app->running && input__poll(&event) == BRUCE_OK) maps__handle(app, &event);
        maps__expire_hint(app);
        if (tiles__check_network(&app->tiles)) app->dirty = true;
        if (display__viewport_changed()) app->dirty = true;

        int width = display__width();
        int height = display__height();
        if (width <= 0 || height <= 0) {
            if (display__await_redraw()) app->dirty = true;
            else runtime__delay(MAPS_IDLE_DELAY_MS);
            continue;
        }

        tile_key_t next = {0};
        size_t pending = map_view__next_pending(&app->view, &app->tiles, width, height, &next);
        if (app->dirty) {
            maps__update_status(app, pending);
            app->dirty = !maps__draw(app, width, height);
        } else if (pending > 0) {
            tiles__load(&app->tiles, next);
            app->dirty = true;
        } else {
            runtime__delay(MAPS_IDLE_DELAY_MS);
        }
    }
}

int app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    maps_t *app = memory__calloc(1, sizeof(maps_t));
    if (app == NULL) return 1;

    settings__load(&app->settings);
    map_view__set(&app->view, app->settings.lat_e6 / 1e6, app->settings.lon_e6 / 1e6, app->settings.zoom);
    app->start_view = app->view;
    int screen_width = display__screen_width();
    int screen_height = display__screen_height();
    int screen = screen_width > screen_height ? screen_width : screen_height;
    size_t min_slots = map_view__max_visible_tiles(screen, screen) + 2;
    if (!tiles__init(
            &app->tiles, min_slots, app->settings.tile_dir, app->settings.tile_url, app->settings.online
        )) {
        dialog__message(BRUCE_DIALOG_ERROR, "Maps", "Not enough memory");
        memory__free(app);
        return 1;
    }

    app->running = true;
    app->dirty = true;
    maps__run(app);

    maps__save_view(app);
    tiles__deinit(&app->tiles);
    memory__free(app);
    return 0;
}
