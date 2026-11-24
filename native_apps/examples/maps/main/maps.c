#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bruce_sdk.h"
#include "geo.h"
#include "hud.h"
#include "map_view.h"
#include "route.h"
#include "search.h"
#include "settings.h"
#include "tiles.h"
#include "view3d.h"

#define MAPS_IDLE_DELAY_MS 30
#define MAPS_PLACE_ZOOM 16
#define MAPS_LONG_PRESS_MS 500
#define MAPS_HINT_MS 2000
#define MAPS_PENDING_MAX 16
#define MAPS_GPS_POLL_MS 1000
#define MAPS_GPS_NO_DATA_MS 10000
#define MAPS_GPS_FIX_SATELLITES 4
#define MAPS_NAV_ZOOM 16
/* Off the route by more than this for a few fixes in a row: route again. */
#define MAPS_OFF_ROUTE_M 50
#define MAPS_OFF_ROUTE_FIXES 3
#define MAPS_REROUTE_MS 15000
#define MAPS_ARRIVED_M 25
#define MAPS_ARRIVED_HINT_MS 5000
#define MAPS_TURN_DEG 15.0f
/* Turning the device turns the 3D view, in steps of at least this. */
#define MAPS_MOTION_POLL_MS 100
#define MAPS_MOTION_STEP_DEG 2.0f

typedef struct {
    settings_t settings;
    map_view_t view;
    view3d_t view3d;
    map_view_t start_view;
    tiles_t tiles;
    hud_state_t hud;
    uint64_t hint_until_ms;
    search_t search;
    char place[SEARCH_NAME_MAX];
    bool running;
    bool dirty;
    bool stalled; /* Every pending tile waits for a download slot. */
    int32_t touch_x;
    int32_t touch_y;
    bool touch_on_button;
    bool gps_available;
    bool gps;            /* Turned on by My location. */
    bool locate_pending; /* Centre on the first fix. */
    uint64_t gps_at;
    uint64_t gps_started_at;
    uint64_t motion_at;
    bool motion_have;  /* motion_yaw holds the last reading. */
    float motion_yaw;  /* Counterclockwise. */
    float motion_turn; /* Turned since the view last followed. */
    char gps_text[40];
    /* What the running search is for. */
    enum { MAPS_FIND_PLACE, MAPS_FIND_DESTINATION, MAPS_FIND_START } finding;
    /* Directions: where to, from where, and how. */
    double to_lat, to_lon;
    char to_name[SEARCH_NAME_MAX];
    double from_lat, from_lon;
    bool from_gps;       /* From my location: the route is navigated live. */
    route_profile_t profile;
    route_request_t route_request;
    bool route_wait_fix; /* Routing from my location once there is a fix. */
    route_t route;       /* point_count 0 when there is none. */
    bool navigating;
    route_progress_t progress;
    size_t shown_step;   /* Without GPS: the step the banner shows. */
    int off_route_fixes;
    uint64_t rerouted_at;
    char banner_distance[16];
    char banner_text[ROUTE_TEXT_MAX];
    char banner_then[ROUTE_TEXT_MAX];
    char summary[40];
} maps_t;

static int32_t maps__to_e6(double degrees) { return (int32_t)floor(degrees * 1000000.0 + 0.5); }

static void maps__pan(maps_t *app, int dx, int dy) {
    if (app->view3d.on) view3d__rotate_pan(&app->view3d, &dx, &dy);
    map_view__pan(&app->view, dx, dy);
    app->hud.following = false;
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

static void maps__show_hint(maps_t *app, const char *icon, const char *line, const char *tip, uint32_t ms) {
    app->hud.hint_icon = icon;
    app->hud.hint[0] = line;
    app->hud.hint[1] = tip;
    app->hint_until_ms = runtime__now() + ms;
    app->dirty = true;
}

static void maps__set_nav_mode(maps_t *app, hud_nav_mode_t mode, bool from_menu) {
    app->hud.nav_mode = mode;
    maps__show_hint(app, hud__nav_icon(mode), s_nav_hints[mode], from_menu ? "Tip: hold Select" : NULL, MAPS_HINT_MS);
}

static void maps__expire_hint(maps_t *app) {
    if (app->hud.hint[0] == NULL || runtime__now() < app->hint_until_ms) return;
    app->hud.hint[0] = app->hud.hint[1] = NULL;
    app->dirty = true;
}

static void maps__show_place(maps_t *app, double lat, double lon, const char *name) {
    int zoom = app->view.zoom > MAPS_PLACE_ZOOM ? app->view.zoom : MAPS_PLACE_ZOOM;
    map_view__set(&app->view, lat, lon, zoom);
    app->hud.following = false;
    app->hud.pin = true;
    app->hud.pin_lat = lat;
    app->hud.pin_lon = lon;
    snprintf(app->place, sizeof(app->place), "%s", name);
    app->hud.place = app->place;
    app->dirty = true;
}

static void maps__clear_pin(maps_t *app) {
    app->hud.pin = false;
    app->hud.place = NULL;
}

/* "50.06, 19.94" reads as one name; the search box cuts names at the first comma. */
static void maps__coordinates_name(const char *text, char *out, size_t capacity) {
    size_t len = 0;
    for (const char *c = text; *c != '\0' && len + 1 < capacity; ++c) {
        char ch = *c == ',' ? ' ' : *c;
        if (ch == ' ' && (len == 0 || out[len - 1] == ' ')) continue;
        out[len++] = ch;
    }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = '\0';
}

static double maps__distance_m(const maps_t *app, const search_place_t *place) {
    double lat = 0, lon = 0;
    map_view__center(&app->view, &lat, &lon);
    const double rad = M_PI / 180.0;
    double a = pow(sin((place->lat - lat) * rad / 2), 2) +
               cos(lat * rad) * cos(place->lat * rad) * pow(sin((place->lon - lon) * rad / 2), 2);
    return 2 * 6371000.0 * asin(sqrt(a));
}

static void maps__directions_from(maps_t *app);
static void maps__directions_mode(maps_t *app);

/* A place from search: shown with a pin, or an end of the route being planned. */
static void maps__place_found(maps_t *app, double lat, double lon, const char *name) {
    if (app->finding == MAPS_FIND_DESTINATION) {
        app->to_lat = lat;
        app->to_lon = lon;
        snprintf(app->to_name, sizeof(app->to_name), "%s", name);
        maps__directions_from(app);
    } else if (app->finding == MAPS_FIND_START) {
        app->from_lat = lat;
        app->from_lon = lon;
        app->from_gps = false;
        maps__directions_mode(app);
    } else {
        maps__show_place(app, lat, lon, name);
    }
}

static void maps__pick_place(maps_t *app, const search_place_t *places, size_t count) {
    if (count == 1) {
        maps__place_found(app, places[0].lat, places[0].lon, places[0].name);
        return;
    }
    bruce_dialog_choice_t choices[SEARCH_RESULTS_MAX];
    char distances[SEARCH_RESULTS_MAX][12];
    for (size_t i = 0; i < count; ++i) {
        route__format_distance(distances[i], sizeof(distances[i]), maps__distance_m(app, &places[i]));
        choices[i] = (bruce_dialog_choice_t){
            .label = places[i].name, .icon_name = "map-marker", .right_text = distances[i]};
    }
    size_t selected = 0;
    if (dialog__choice("Places", NULL, choices, count, &selected) == BRUCE_OK) {
        maps__place_found(app, places[selected].lat, places[selected].lon, places[selected].name);
    }
}

static void maps__search_finished(maps_t *app, bruce_result_t result, const search_place_t *places, size_t count) {
    app->dirty = true;
    if (result != BRUCE_OK) {
        const char *message = net__is_online() ? "Search failed, try again later" : "No internet connection";
        dialog__message(BRUCE_DIALOG_WARNING, "Search", message);
    } else if (count == 0) {
        dialog__message(BRUCE_DIALOG_INFO, "Search", "No places found");
    } else {
        maps__pick_place(app, places, count);
    }
}

/* Coordinates jump straight there; anything else asks the search server. */
static void maps__search_for(maps_t *app, int finding, const char *title) {
    char text[SEARCH_QUERY_MAX] = {0};
    app->dirty = true;
    app->finding = finding;
    if (dialog__text_input(title, "place or lat, lon", NULL, false, text, sizeof(text)) != BRUCE_OK) return;
    double lat = 0, lon = 0;
    if (search__parse_coordinates(text, &lat, &lon)) {
        char name[SEARCH_QUERY_MAX];
        maps__coordinates_name(text, name, sizeof(name));
        maps__place_found(app, lat, lon, name);
        return;
    }
    if (text[strspn(text, " ")] == '\0') return;
    bruce_result_t result = search__start(
        &app->search, app->settings.search_url, text, &app->view, display__width(), display__height()
    );
    if (result != BRUCE_OK) maps__search_finished(app, result, NULL, 0);
}

static void maps__search(maps_t *app) { maps__search_for(app, MAPS_FIND_PLACE, "Search"); }

static void maps__poll_search(maps_t *app) {
    if (app->search.state == SEARCH_IDLE) return;
    search_place_t places[SEARCH_RESULTS_MAX];
    size_t count = 0;
    bruce_result_t result = search__poll(&app->search, places, &count);
    if (result != BRUCE_ERR_BUSY) maps__search_finished(app, result, places, count);
}

/* Turns GPS on, then centres on the position and keeps following it until the map is panned. */
static void maps__locate(maps_t *app) {
    if (!app->gps) {
        app->gps = true;
        app->gps_at = app->gps_started_at = 0;
        app->gps_text[0] = '\0';
    }
    if (!app->hud.me) {
        app->locate_pending = true;
        app->dirty = true;
        return;
    }
    app->locate_pending = false;
    int zoom = app->view.zoom > MAPS_PLACE_ZOOM ? app->view.zoom : MAPS_PLACE_ZOOM;
    map_view__set(&app->view, app->hud.me_lat, app->hud.me_lon, zoom);
    app->hud.following = true;
    app->dirty = true;
}

static void maps__gps_off(maps_t *app) {
    /* The route stays, to step through by hand. */
    if (app->navigating) app->shown_step = app->progress.next;
    app->navigating = app->route_wait_fix = false;
    app->gps = app->locate_pending = app->hud.me = app->hud.following = app->hud.speed = false;
    app->hud.gps_status = NULL;
    app->dirty = true;
}

/* ---- Directions ------------------------------------------------------------ */

static bool maps__has_route(const maps_t *app) { return app->route.point_count > 0; }

static void maps__set_text(maps_t *app, char *field, size_t capacity, const char *text) {
    if (strcmp(field, text) == 0) return;
    snprintf(field, capacity, "%s", text);
    app->dirty = true;
}

/* The banner and summary from the progress along the route, or the shown step without GPS. */
static void maps__update_banner(maps_t *app) {
    if (!maps__has_route(app)) {
        app->hud.route = NULL;
        app->hud.banner_step = NULL;
        app->hud.summary = NULL;
        return;
    }
    const route_t *route = &app->route;
    size_t step = app->navigating ? app->progress.next : app->shown_step;
    char distance[16], text[ROUTE_TEXT_MAX], then[ROUTE_TEXT_MAX] = "", length[16], time[16], summary[40];
    double left_m = app->navigating ? app->progress.left_m : route__length_m(route);
    double left_s = app->navigating ? app->progress.left_s : route__duration_s(route);
    if (app->navigating) route__format_countdown(distance, sizeof(distance), app->progress.to_next_m);
    else snprintf(distance, sizeof(distance), "%u/%u", (unsigned)step + 1, (unsigned)route->step_count);
    route__instruction(&route->steps[step], text, sizeof(text));
    if (step + 1 < route->step_count) route__instruction(&route->steps[step + 1], then, sizeof(then));
    route__format_distance(length, sizeof(length), left_m);
    route__format_duration(time, sizeof(time), left_s);
    snprintf(summary, sizeof(summary), "%s, %s", length, time);
    if (app->hud.banner_step != &route->steps[step]) app->dirty = true;
    app->hud.route = route;
    app->hud.banner_step = &route->steps[step];
    maps__set_text(app, app->banner_distance, sizeof(app->banner_distance), distance);
    maps__set_text(app, app->banner_text, sizeof(app->banner_text), text);
    maps__set_text(app, app->banner_then, sizeof(app->banner_then), then);
    maps__set_text(app, app->summary, sizeof(app->summary), summary);
    app->hud.banner_distance = app->banner_distance;
    app->hud.banner_text = app->banner_text;
    app->hud.banner_then = app->banner_then[0] != '\0' ? app->banner_then : NULL;
    app->hud.summary = app->summary;
}

static void maps__end_route(maps_t *app) {
    route__cancel(&app->route_request);
    route__free(&app->route);
    app->navigating = app->route_wait_fix = false;
    app->hud.route = NULL;
    maps__update_banner(app);
    app->dirty = true;
}

/* Zooms so the whole route fits on screen, centred on it. */
static void maps__fit_route(maps_t *app) {
    geo_point_t low = app->route.points[0], high = low;
    for (size_t i = 1; i < app->route.point_count; ++i) {
        geo_point_t p = app->route.points[i];
        if (p.x < low.x) low.x = p.x;
        if (p.y < low.y) low.y = p.y;
        if (p.x > high.x) high.x = p.x;
        if (p.y > high.y) high.y = p.y;
    }
    int width = display__width() * 3 / 4, height = display__height() / 2;
    int zoom = ROUTE_ZOOM;
    while (zoom > GEO_MIN_ZOOM &&
           (((high.x - low.x) >> (ROUTE_ZOOM - zoom)) > width || ((high.y - low.y) >> (ROUTE_ZOOM - zoom)) > height)) {
        zoom--;
    }
    if (zoom > GEO_MAX_ZOOM - 1) zoom = GEO_MAX_ZOOM - 1;
    app->view.zoom = zoom;
    app->view.center.x = (int32_t)(((int64_t)low.x + high.x) / 2) >> (ROUTE_ZOOM - zoom);
    app->view.center.y = (int32_t)(((int64_t)low.y + high.y) / 2) >> (ROUTE_ZOOM - zoom);
}

static void maps__show_step(maps_t *app, size_t step) {
    if (!maps__has_route(app) || step >= app->route.step_count) return;
    app->shown_step = step;
    geo_point_t point = app->route.points[app->route.steps[step].point];
    int zoom = app->view.zoom > MAPS_NAV_ZOOM ? app->view.zoom : MAPS_NAV_ZOOM;
    app->view.zoom = zoom;
    app->view.center.x = point.x >> (ROUTE_ZOOM - zoom);
    app->view.center.y = point.y >> (ROUTE_ZOOM - zoom);
    app->hud.following = false;
    maps__update_banner(app);
    app->dirty = true;
}

static void maps__route_failed(maps_t *app, bruce_result_t result) {
    app->dirty = true;
    /* A failed reroute keeps the old route; the next fix off it tries again. */
    if (app->navigating) return;
    const char *message = result == BRUCE_ERR_NOT_FOUND ? "No route found"
                          : net__is_online()            ? "Routing failed, try again later"
                                                        : "No internet connection";
    dialog__message(BRUCE_DIALOG_WARNING, "Directions", message);
}

static void maps__route_ready(maps_t *app, route_t *route) {
    bool rerouted = app->navigating;
    route__free(&app->route);
    app->route = *route;
    app->progress = (route_progress_t){0};
    app->off_route_fixes = 0;
    app->shown_step = 0;
    app->hud.pin = true;
    app->hud.pin_lat = app->to_lat;
    app->hud.pin_lon = app->to_lon;
    snprintf(app->place, sizeof(app->place), "%s", app->to_name);
    app->hud.place = app->place;
    app->navigating = app->from_gps && app->gps;
    if (app->navigating) {
        route__track(&app->route, app->hud.me_lat, app->hud.me_lon, 0, &app->progress);
        if (!rerouted) {
            int zoom = app->view.zoom > MAPS_NAV_ZOOM ? app->view.zoom : MAPS_NAV_ZOOM;
            map_view__set(&app->view, app->hud.me_lat, app->hud.me_lon, zoom);
            app->hud.following = true;
        }
    } else {
        maps__fit_route(app);
        app->hud.following = false;
    }
    maps__update_banner(app);
    app->dirty = true;
}

static void maps__route_request(maps_t *app) {
    if (app->from_gps) {
        app->from_lat = app->hud.me_lat;
        app->from_lon = app->hud.me_lon;
    }
    bruce_result_t result = route__start(&app->route_request, app->settings.route_url, app->profile, app->from_lat,
                                         app->from_lon, app->to_lat, app->to_lon);
    if (result != BRUCE_OK) maps__route_failed(app, result);
    app->dirty = true;
}

/* From my location waits for the first fix. */
static void maps__route_start(maps_t *app) {
    if (app->from_gps && !app->hud.me) {
        if (!app->gps) {
            app->gps = true;
            app->gps_at = app->gps_started_at = 0;
            app->gps_text[0] = '\0';
        }
        app->route_wait_fix = true;
        app->dirty = true;
        return;
    }
    maps__route_request(app);
}

static void maps__poll_route(maps_t *app) {
    if (app->route_request.state == ROUTE_IDLE) return;
    route_t route;
    bruce_result_t result = route__poll(&app->route_request, &route);
    if (result == BRUCE_ERR_BUSY) return;
    if (result == BRUCE_OK) maps__route_ready(app, &route);
    else maps__route_failed(app, result);
}

static void maps__directions_mode(maps_t *app) {
    static const char *const labels[ROUTE_PROFILE_COUNT] = {"Car", "Bike", "Walk"};
    bruce_dialog_choice_t choices[ROUTE_PROFILE_COUNT];
    for (int i = 0; i < ROUTE_PROFILE_COUNT; ++i) choices[i] = (bruce_dialog_choice_t){.label = labels[i]};
    size_t selected = app->profile;
    app->dirty = true;
    if (dialog__choice("Travel by", NULL, choices, ROUTE_PROFILE_COUNT, &selected) != BRUCE_OK) return;
    app->profile = (route_profile_t)selected;
    maps__route_start(app);
}

static void maps__directions_from(maps_t *app) {
    enum { FROM_ME, FROM_CENTRE, FROM_SEARCH };
    bruce_dialog_choice_t choices[3];
    int actions[3];
    size_t count = 0;
    if (app->gps_available) {
        choices[count] = (bruce_dialog_choice_t){.label = "My location", .icon_name = "crosshairs-gps"};
        actions[count++] = FROM_ME;
    }
    choices[count] = (bruce_dialog_choice_t){.label = "Map centre", .icon_name = "crosshairs"};
    actions[count++] = FROM_CENTRE;
    choices[count] = (bruce_dialog_choice_t){.label = "Search", .icon_name = "magnify"};
    actions[count++] = FROM_SEARCH;
    size_t selected = 0;
    app->dirty = true;
    if (dialog__choice("From", NULL, choices, count, &selected) != BRUCE_OK) return;
    app->from_gps = actions[selected] == FROM_ME;
    if (actions[selected] == FROM_SEARCH) {
        maps__search_for(app, MAPS_FIND_START, "From");
        return;
    }
    if (actions[selected] == FROM_CENTRE) map_view__center(&app->view, &app->from_lat, &app->from_lon);
    maps__directions_mode(app);
}

/* Where to: the pinned place, the map centre, or a search. */
static void maps__directions(maps_t *app) {
    enum { TO_PIN, TO_CENTRE, TO_SEARCH };
    char pin_label[48] = "";
    bruce_dialog_choice_t choices[3];
    int actions[3];
    size_t count = 0;
    if (app->hud.pin && app->hud.place != NULL) {
        snprintf(pin_label, sizeof(pin_label), "%.*s", (int)strcspn(app->hud.place, ","), app->hud.place);
        choices[count] = (bruce_dialog_choice_t){.label = pin_label, .icon_name = "map-marker"};
        actions[count++] = TO_PIN;
    }
    choices[count] = (bruce_dialog_choice_t){.label = "Search", .icon_name = "magnify"};
    actions[count++] = TO_SEARCH;
    choices[count] = (bruce_dialog_choice_t){.label = "Map centre", .icon_name = "crosshairs"};
    actions[count++] = TO_CENTRE;
    size_t selected = 0;
    app->dirty = true;
    if (dialog__choice("To", NULL, choices, count, &selected) != BRUCE_OK) return;
    if (actions[selected] == TO_SEARCH) {
        maps__search_for(app, MAPS_FIND_DESTINATION, "To");
        return;
    }
    if (actions[selected] == TO_PIN) {
        app->to_lat = app->hud.pin_lat;
        app->to_lon = app->hud.pin_lon;
        snprintf(app->to_name, sizeof(app->to_name), "%s", app->hud.place);
    } else {
        map_view__center(&app->view, &app->to_lat, &app->to_lon);
        snprintf(app->to_name, sizeof(app->to_name), "Map centre");
    }
    maps__directions_from(app);
}

/* Every step with the distance to the one after; picking one shows it. */
static void maps__directions_list(maps_t *app) {
    const route_t *route = &app->route;
    size_t count = route->step_count;
    bruce_dialog_choice_t *choices = memory__calloc(count, sizeof(*choices));
    char (*texts)[ROUTE_TEXT_MAX] = memory__calloc(count, ROUTE_TEXT_MAX);
    char (*distances)[12] = memory__calloc(count, 12);
    app->dirty = true;
    if (choices == NULL || texts == NULL || distances == NULL) {
        dialog__message(BRUCE_DIALOG_ERROR, "Directions", "Not enough memory");
    } else {
        for (size_t i = 0; i < count; ++i) {
            route__instruction(&route->steps[i], texts[i], ROUTE_TEXT_MAX);
            if (i + 1 < count) {
                float meters = route->along[route->steps[i + 1].point] - route->along[route->steps[i].point];
                route__format_distance(distances[i], 12, meters);
            }
            choices[i] = (bruce_dialog_choice_t){.label = texts[i], .right_text = distances[i]};
        }
        size_t selected = app->navigating ? app->progress.next : app->shown_step;
        if (dialog__choice("Directions", NULL, choices, count, &selected) == BRUCE_OK) {
            if (app->navigating) {
                geo_point_t point = route->points[route->steps[selected].point];
                app->view.center.x = point.x >> (ROUTE_ZOOM - app->view.zoom);
                app->view.center.y = point.y >> (ROUTE_ZOOM - app->view.zoom);
                app->hud.following = false;
            } else {
                maps__show_step(app, selected);
            }
        }
    }
    memory__free(choices);
    memory__free(texts);
    memory__free(distances);
}

/* Each fix: where along the route, arrival, and a new route when off it for a while. */
static void maps__navigate(maps_t *app, uint64_t now) {
    if (app->route_wait_fix) {
        app->route_wait_fix = false;
        maps__route_request(app);
        return;
    }
    if (!app->navigating) return;
    route_progress_t progress;
    route__track(&app->route, app->hud.me_lat, app->hud.me_lon, app->progress.segment, &progress);
    if (progress.left_m < MAPS_ARRIVED_M && progress.off_m < MAPS_ARRIVED_M) {
        maps__end_route(app);
        maps__show_hint(app, "map-marker", "You have arrived", NULL, MAPS_ARRIVED_HINT_MS);
        return;
    }
    app->off_route_fixes = progress.off_m > MAPS_OFF_ROUTE_M ? app->off_route_fixes + 1 : 0;
    if (app->off_route_fixes >= MAPS_OFF_ROUTE_FIXES && app->route_request.state == ROUTE_IDLE &&
        (app->rerouted_at == 0 || now - app->rerouted_at >= MAPS_REROUTE_MS)) {
        app->rerouted_at = now;
        maps__route_request(app);
    }
    app->progress = progress;
    maps__update_banner(app);
}

/* Steps towards a fix: data from the receiver, the time, then enough satellites. */
static void maps__gps_status(maps_t *app, const bruce_gps_fix_t *fix, uint64_t now) {
    char text[sizeof(app->gps_text)] = "";
    unsigned used = fix->satellites < MAPS_GPS_FIX_SATELLITES ? fix->satellites : MAPS_GPS_FIX_SATELLITES;
    if (fix->fix) {
        text[0] = '\0';
    } else if (!fix->receiving) {
        bool slow = now - app->gps_started_at >= MAPS_GPS_NO_DATA_MS;
        snprintf(text, sizeof(text), "GPS 1/3: %s", slow ? "no data" : "starting");
    } else if (!fix->time_valid) {
        snprintf(text, sizeof(text), "GPS 2/3: time sync, %u seen", fix->satellites_in_view);
    } else {
        snprintf(text, sizeof(text), "GPS 3/3: %u/%d locked, %u seen", used, MAPS_GPS_FIX_SATELLITES,
                 fix->satellites_in_view);
    }
    if (strcmp(text, app->gps_text) != 0) app->dirty = true;
    snprintf(app->gps_text, sizeof(app->gps_text), "%s", text);
    app->hud.gps_status = app->gps_text[0] != '\0' ? app->gps_text : NULL;
}

/* Keeps the map on the position. An e-paper map stays still while the
 * position is in the middle half of the screen, so only the dot redraws; when
 * it leaves, the map moves to put it at the far side of that box, so the
 * screen shows more of the way ahead. */
static void maps__follow(maps_t *app, geo_point_t me) {
    int32_t dx = me.x - app->view.center.x;
    int32_t dy = me.y - app->view.center.y;
    int32_t box_x = display__width() / 4;
    int32_t box_y = display__height() / 4;
    bool off_screen = abs(dx) > 2 * box_x || abs(dy) > 2 * box_y;
    if (!display__is_epaper() || off_screen || app->view3d.on) {
        app->view.center = me;
        return;
    }
    int32_t move_x = dx > box_x ? dx + box_x : dx < -box_x ? dx - box_x : 0;
    int32_t move_y = dy > box_y ? dy + box_y : dy < -box_y ? dy - box_y : 0;
    if (move_x != 0 || move_y != 0) map_view__pan(&app->view, move_x, move_y);
}

static void maps__poll_gps(maps_t *app) {
    uint64_t now = runtime__now();
    if (!app->gps || now < app->gps_at) return;
    if (app->gps_started_at == 0) app->gps_started_at = now;
    app->gps_at = now + MAPS_GPS_POLL_MS;
    bruce_gps_fix_t fix;
    if (gps__read(&fix) != BRUCE_OK) {
        maps__gps_off(app);
        app->gps_available = app->hud.locate_button = false;
        return;
    }
    geo_point_t before = geo__from_lat_lon(app->hud.me_lat, app->hud.me_lon, app->view.zoom);
    geo_point_t after = geo__from_lat_lon(fix.latitude, fix.longitude, app->view.zoom);
    bool moved = before.x != after.x || before.y != after.y;
    maps__gps_status(app, &fix, now);
    int speed = (int)(fix.speed_kmh + 0.5f);
    if (fix.fix != app->hud.me || (fix.fix && (moved || speed != app->hud.speed_kmh))) app->dirty = true;
    app->hud.me = app->hud.speed = fix.fix;
    if (!fix.fix) return;
    app->hud.me_lat = fix.latitude;
    app->hud.me_lon = fix.longitude;
    app->hud.speed_kmh = speed;
    /* The 3D view faces the way of travel; standing still keeps the last heading. */
    if (fix.speed_kmh >= 3.0f && view3d__set_heading(&app->view3d, fix.course_deg) && app->view3d.on) {
        app->dirty = true;
    }
    if (app->locate_pending) maps__locate(app);
    else if (app->hud.following) maps__follow(app, after);
    maps__navigate(app, now);
}

/* The 3D view turns with the device, GPS or not; moving with GPS still sets it to the way of travel. */
static void maps__poll_motion(maps_t *app) {
    uint64_t now = runtime__now();
    if (!app->view3d.on) {
        app->motion_have = false;
        return;
    }
    if (now < app->motion_at) return;
    app->motion_at = now + MAPS_MOTION_POLL_MS;
    bruce_motion_t motion;
    if (motion__read(&motion) != BRUCE_OK || !motion.ready) {
        app->motion_have = false;
        return;
    }
    if (app->motion_have) {
        float change = fmodf(motion.yaw_deg - app->motion_yaw + 540.0f, 360.0f) - 180.0f;
        /* Yaw turns counterclockwise, the heading clockwise. */
        app->motion_turn -= change;
        if (fabsf(app->motion_turn) >= MAPS_MOTION_STEP_DEG) {
            view3d__turn(&app->view3d, app->motion_turn);
            app->motion_turn = 0;
            app->dirty = true;
        }
    }
    app->motion_yaw = motion.yaw_deg;
    app->motion_have = true;
}

static void maps__toggle_online(maps_t *app) {
    tiles__set_online(&app->tiles, !app->tiles.online);
    settings__save_online(app->tiles.online);
}

static void maps__toggle_big(maps_t *app) {
    app->view.big = !app->view.big;
    settings__save_big(app->view.big);
}

static void maps__toggle_3d(maps_t *app) {
    app->view3d.on = !app->view3d.on;
    app->hud.turn_buttons = app->view3d.on && input__has_touch();
    settings__save_3d(app->view3d.on);
}

/* Moving with GPS turns it back to the way of travel. */
static void maps__turn(maps_t *app, float degrees) {
    if (!app->view3d.on) return;
    view3d__turn(&app->view3d, degrees);
    app->dirty = true;
}

static void maps__menu(maps_t *app) {
    enum { NAV_MODE, SEARCH, DIRECTIONS, STEPS, END_ROUTE, LOCATE, GPS_OFF, CLEAR_PIN, VIEW_3D, ONLINE, BIG, EXIT, BACK };
    hud_nav_mode_t next_mode = maps__next_nav_mode(app);
    char nav_label[32];
    snprintf(nav_label, sizeof(nav_label), "Change to %s", s_nav_names[next_mode]);
    const bruce_dialog_choice_t all[] = {
        [NAV_MODE] = {.label = nav_label, .icon_name = hud__nav_icon(next_mode)},
        [SEARCH] = {.label = "Search", .icon_name = "magnify"},
        [DIRECTIONS] = {.label = "Directions", .icon_name = "compass"},
        [STEPS] = {.label = "All directions", .icon_name = "file-document"},
        [END_ROUTE] = {.label = "End route", .icon_name = "close"},
        [LOCATE] = {.label = "My location", .icon_name = "crosshairs-gps"},
        [GPS_OFF] = {.label = "Turn GPS off", .icon_name = "crosshairs"},
        [CLEAR_PIN] = {.label = "Clear pin", .icon_name = "map-marker"},
        [VIEW_3D] = {.label = "3D view", .icon_name = "video-3d", .right_text = app->view3d.on ? "On" : "Off"},
        [ONLINE] = {.label = "Online tiles", .icon_name = "earth",
                    .right_text = app->tiles.online ? "On" : "Off"},
        [BIG] = {.label = "Bigger text", .icon_name = "format-size", .right_text = app->view.big ? "On" : "Off"},
        [EXIT] = {.label = "Exit", .icon_name = "power"},
        [BACK] = {.label = "Back", .icon_name = "arrow-left"},
    };
    bruce_dialog_choice_t choices[sizeof(all) / sizeof(all[0])];
    int actions[sizeof(all) / sizeof(all[0])];
    size_t count = 0;
    for (int i = 0; i < (int)(sizeof(all) / sizeof(all[0])); ++i) {
        if (i == CLEAR_PIN && !app->hud.pin) continue;
        if ((i == STEPS || i == END_ROUTE) && !maps__has_route(app)) continue;
        if (i == LOCATE && !app->gps_available) continue;
        if (i == GPS_OFF && !app->gps) continue;
        /* Keyboards and touch screens pan and zoom directly; the Prev/Next mode is for button boards. */
        if (i == NAV_MODE && (input__has_keyboard() || input__has_touch())) continue;
        choices[count] = all[i];
        actions[count++] = i;
    }
    size_t selected = 0;
    if (dialog__choice("Maps", NULL, choices, count, &selected) == BRUCE_OK) {
        switch (actions[selected]) {
            case NAV_MODE: maps__set_nav_mode(app, next_mode, true); break;
            case SEARCH: maps__search(app); break;
            case DIRECTIONS: maps__directions(app); break;
            case STEPS: maps__directions_list(app); break;
            case END_ROUTE: maps__end_route(app); break;
            case LOCATE: maps__locate(app); break;
            case GPS_OFF: maps__gps_off(app); break;
            case CLEAR_PIN: maps__clear_pin(app); break;
            case VIEW_3D: maps__toggle_3d(app); break;
            case ONLINE: maps__toggle_online(app); break;
            case BIG: maps__toggle_big(app); break;
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

static bool maps__hit(hud_rect_t rect, int32_t x, int32_t y) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static bool maps__hit_button(hud_button_t button, int32_t x, int32_t y) {
    return maps__hit(hud__button_rect(button, display__width(), display__height()), x, y);
}

/* Drag pans; the +/- buttons appear with the first touch, so keypad boards never see them. */
static void maps__touch(maps_t *app, const bruce_input_event_t *event) {
    if (event->action == BRUCE_INPUT_PRESS) {
        app->dirty |= !app->hud.zoom_buttons;
        app->hud.zoom_buttons = true;
        app->touch_on_button = true;
        hud_rect_t search = hud__search_rect(&app->view, &app->hud, display__width());
        if (app->hud.menu_button && maps__hit_button(HUD_BUTTON_MENU, event->x, event->y)) maps__menu(app);
        else if (app->hud.turn_buttons && maps__hit_button(HUD_BUTTON_TURN_LEFT, event->x, event->y)) {
            maps__turn(app, -MAPS_TURN_DEG);
        } else if (app->hud.turn_buttons && maps__hit_button(HUD_BUTTON_TURN_RIGHT, event->x, event->y)) {
            maps__turn(app, MAPS_TURN_DEG);
        } else if (maps__hit_button(HUD_BUTTON_ZOOM_IN, event->x, event->y)) maps__zoom(app, 1);
        else if (maps__hit_button(HUD_BUTTON_ZOOM_OUT, event->x, event->y)) maps__zoom(app, -1);
        else if (app->hud.locate_button && maps__hit_button(HUD_BUTTON_LOCATE, event->x, event->y)) {
            maps__locate(app);
        } else if (maps__has_route(app) && maps__hit(hud__banner_rect(display__width()), event->x, event->y)) {
            if (!app->navigating) maps__show_step(app, (app->shown_step + 1) % app->route.step_count);
        } else if (maps__hit(search, event->x, event->y)) maps__search(app);
        else app->touch_on_button = false;
    } else if (event->action == BRUCE_INPUT_CHANGE && !app->touch_on_button) {
        maps__pan(app, app->touch_x - event->x, app->touch_y - event->y);
    }
    app->touch_x = event->x;
    app->touch_y = event->y;
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
        case BRUCE_INPUT_CODE_BACK:
            /* Esc on a keyboard opens the menu, which has End route and Exit; a Back button keeps its shortcuts. */
            if (app->search.state != SEARCH_IDLE) search__cancel(&app->search);
            else if (event->type == BRUCE_INPUT_KEY) maps__menu(app);
            else if (app->route_request.state != ROUTE_IDLE || app->route_wait_fix || maps__has_route(app)) {
                maps__end_route(app);
            } else app->running = false;
            app->dirty = true;
            break;
        case BRUCE_INPUT_CODE_UP: maps__pan(app, 0, -step_y); break;
        case BRUCE_INPUT_CODE_DOWN: maps__pan(app, 0, step_y); break;
        case BRUCE_INPUT_CODE_LEFT: maps__pan(app, -step_x, 0); break;
        case BRUCE_INPUT_CODE_RIGHT: maps__pan(app, step_x, 0); break;
        case BRUCE_INPUT_CODE_ZOOM_IN:
        case '+':
        case '=': maps__zoom(app, 1); break;
        case BRUCE_INPUT_CODE_ZOOM_OUT:
        case '-': maps__zoom(app, -1); break;
        case 'q':
        case 'Q': maps__turn(app, -MAPS_TURN_DEG); break;
        case 'e':
        case 'E': maps__turn(app, MAPS_TURN_DEG); break;
        case BRUCE_INPUT_CODE_NEXT: maps__step(app, 1); break;
        case BRUCE_INPUT_CODE_PREV: maps__step(app, -1); break;
        case BRUCE_INPUT_CODE_MENU: maps__menu(app); break;
        case BRUCE_INPUT_CODE_SELECT: maps__select(app, event); break;
        default: break;
    }
}

/* Redraws only when the text changes; the strings are literals, so pointers compare. */
static void maps__update_status(maps_t *app, size_t loading) {
    const char *before = app->hud.status;
    bool searching = app->search.state == SEARCH_RUNNING;
    bool routing = app->route_request.state == ROUTE_RUNNING;
    bool connecting = app->search.state == SEARCH_CONNECTING || app->route_request.state == ROUTE_CONNECTING ||
                      (loading == 0 && app->tiles.connecting);
    bool offline = !searching && !connecting && loading == 0 && app->tiles.network_down;
    app->hud.status = searching    ? "Searching"
                      : routing    ? "Routing"
                      : connecting ? "Connecting"
                      : loading > 0 ? "Loading"
                      : offline    ? "Offline"
                                   : NULL;
    app->hud.status_icon = searching    ? "magnify"
                           : routing    ? "compass"
                           : connecting ? "web"
                           : offline    ? "wifi-cancel"
                                        : NULL;
    if (app->hud.status != before) app->dirty = true;
}

static bool maps__draw(maps_t *app, int width, int height) {
    if (display__begin_frame() != BRUCE_OK) return false;
    if (app->view3d.on) {
        /* The ground, route and markers in perspective; the HUD adds the rest. */
        view3d__draw(&app->view3d, &app->view, &app->tiles, &app->hud, width, height);
        hud_state_t flat = app->hud;
        flat.route = NULL;
        flat.pin = flat.me = false;
        flat.following = true;
        hud__draw(&app->view, &flat, width, height);
    } else {
        map_view__draw(&app->view, &app->tiles, width, height);
        hud__draw(&app->view, &app->hud, width, height);
    }
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

typedef struct {
    const map_view_t *view;
    const view3d_t *view3d;
    int width;
    int height;
} maps_screen_t;

static bool maps__keep_download(tile_key_t key, void *context) {
    const maps_screen_t *screen = context;
    if (screen->view3d->on) return view3d__is_near(screen->view3d, key);
    return map_view__is_near(screen->view, key, screen->width, screen->height);
}

/* Nearest first: SD tiles decode at once, the rest start downloading. */
static void maps__load_tiles(maps_t *app, const tile_key_t *keys, size_t count) {
    bool progressed = false;
    for (size_t i = 0; i < count; ++i) {
        tile_load_t result = tiles__load(&app->tiles, keys[i]);
        if (result == TILE_LOADED) {
            app->dirty = true;
            return;
        }
        progressed |= result != TILE_DEFERRED;
    }
    app->stalled = !progressed;
}

static void maps__run(maps_t *app) {
    while (app->running) {
        bruce_input_event_t event;
        while (app->running && input__poll(&event) == BRUCE_OK) maps__handle(app, &event);
        maps__expire_hint(app);
        maps__poll_search(app);
        maps__poll_route(app);
        maps__poll_gps(app);
        maps__poll_motion(app);
        if (tiles__check_network(&app->tiles)) app->dirty = true;
        if (display__viewport_changed()) app->dirty = true;

        int width = display__width();
        int height = display__height();
        if (width <= 0 || height <= 0) {
            if (display__await_redraw()) app->dirty = true;
            else runtime__delay(MAPS_IDLE_DELAY_MS);
            continue;
        }

        maps_screen_t screen = {.view = &app->view, .view3d = &app->view3d, .width = width, .height = height};
        if (tiles__cancel_unless(&app->tiles, maps__keep_download, &screen)) app->stalled = false;
        if (tiles__poll(&app->tiles)) app->stalled = false;

        tile_key_t pending[MAPS_PENDING_MAX];
        size_t count = app->view3d.on
                           ? view3d__pending(&app->view3d, &app->view, &app->tiles, width, height, pending, MAPS_PENDING_MAX)
                           : map_view__pending(&app->view, &app->tiles, width, height, pending, MAPS_PENDING_MAX);
        maps__update_status(app, count + app->tiles.download_count);
        if (app->dirty) {
            app->dirty = !maps__draw(app, width, height);
            app->stalled = false;
        } else if (count > 0 && !app->stalled) {
            maps__load_tiles(app, pending, count);
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
    app->view.big = app->settings.big;
    app->view3d.on = app->settings.view3d;
    app->hud.turn_buttons = app->view3d.on && input__has_touch();
    app->start_view = app->view;
    app->gps_available = gps__is_available();
    app->hud.locate_button = app->gps_available && input__has_touch();
    app->hud.menu_button = input__has_touch();
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
    search__cancel(&app->search);
    maps__end_route(app);
    tiles__deinit(&app->tiles);
    memory__free(app);
    return 0;
}
