#include <math.h>
#include <stdlib.h>

#include "bruce_sdk.h"
#include "geo.h"
#include "hud.h"
#include "map_view.h"
#include "settings.h"
#include "tiles.h"

#define MAPS_LONG_PRESS_MS 500
#define MAPS_IDLE_DELAY_MS 30
#define MAPS_GOTO_ZOOM 15

typedef struct {
    settings_t settings;
    map_view_t view;
    map_view_t start_view;
    tiles_t tiles;
    hud_state_t hud;
    bool running;
    bool dirty;
    int32_t touch_x;
    int32_t touch_y;
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

static void maps__wheel(maps_t *app, int direction) {
    switch (app->hud.wheel_mode) {
        case HUD_WHEEL_ZOOM: maps__zoom(app, direction); break;
        case HUD_WHEEL_PAN_X: maps__pan(app, direction * display__width() / 8, 0); break;
        case HUD_WHEEL_PAN_Y: maps__pan(app, 0, direction * display__height() / 8); break;
    }
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
    enum { GO_TO, ONLINE, ZOOM_IN, ZOOM_OUT, EXIT };
    const bruce_dialog_choice_t choices[] = {
        [GO_TO] = {.label = "Go to coordinates", .icon_name = "map-marker"},
        [ONLINE] = {.label = "Online tiles", .icon_name = "earth",
                    .right_text = app->tiles.online ? "On" : "Off"},
        [ZOOM_IN] = {.label = "Zoom in"},
        [ZOOM_OUT] = {.label = "Zoom out"},
        [EXIT] = {.label = "Exit"},
    };
    size_t selected = 0;
    if (dialog__choice("Maps", NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected) == BRUCE_OK) {
        switch (selected) {
            case GO_TO: maps__go_to(app); break;
            case ONLINE: maps__toggle_online(app); break;
            case ZOOM_IN: map_view__zoom(&app->view, 1); break;
            case ZOOM_OUT: map_view__zoom(&app->view, -1); break;
            case EXIT: app->running = false; break;
        }
    }
    app->dirty = true;
}

/* Encoder boards: a click switches what the wheel does, a long press opens the menu. */
static void maps__select(maps_t *app, const bruce_input_event_t *event) {
    if (event->type == BRUCE_INPUT_KEY || input__wait_long_press(event, MAPS_LONG_PRESS_MS)) {
        maps__menu(app);
        return;
    }
    app->hud.wheel_mode = (app->hud.wheel_mode + 1) % (HUD_WHEEL_PAN_Y + 1);
    app->dirty = true;
}

static void maps__touch(maps_t *app, const bruce_input_event_t *event) {
    if (event->action == BRUCE_INPUT_CHANGE) {
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
        case BRUCE_INPUT_CODE_NEXT: maps__wheel(app, 1); break;
        case BRUCE_INPUT_CODE_PREV: maps__wheel(app, -1); break;
        case BRUCE_INPUT_CODE_MENU: maps__menu(app); break;
        case BRUCE_INPUT_CODE_SELECT: maps__select(app, event); break;
        default: break;
    }
}

static const char *maps__status(const maps_t *app, size_t pending) {
    if (pending > 0) return "Loading";
    if (app->tiles.network_down) return "No Wi-Fi";
    return NULL;
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
            app->hud.status = maps__status(app, pending);
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
