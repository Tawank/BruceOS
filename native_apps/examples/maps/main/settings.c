#include "settings.h"

#include <math.h>
#include <stdio.h>

#include "core_sdk/app_config.h"
#include "core_sdk/display.h"
#include "geo.h"

#define SETTINGS_DEFAULT_TILE_DIR "/sdcard/maps/tiles"
#define SETTINGS_DEFAULT_TILE_URL "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
#define SETTINGS_DEFAULT_SEARCH_URL "https://nominatim.openstreetmap.org/search"
#define SETTINGS_DEFAULT_ROUTE_URL "https://routing.openstreetmap.de"
#define SETTINGS_HISTORY_MAX SEARCH_RESULTS_MAX

/* "lat_e6 lon_e6 name" each. */
static char s_history[SETTINGS_HISTORY_MAX][BRUCE_APP_CONFIG_STRING_MAX_LEN];

void settings__load(settings_t *settings) {
    settings->lat_e6 = app_config__get_int(NULL, "view.lat_e6", 50000000);
    settings->lon_e6 = app_config__get_int(NULL, "view.lon_e6", 15000000);
    settings->zoom = app_config__get_int(NULL, "view.zoom", 4);
    if (settings->zoom < GEO_MIN_ZOOM || settings->zoom > GEO_MAX_ZOOM) settings->zoom = 4;
    settings->online = app_config__get_bool(NULL, "tiles.online", true);
    /* Small map labels are hard to read once dithered to black and white. */
    settings->big = app_config__get_bool(NULL, "view.big", display__is_epaper());
    settings->view3d = app_config__get_bool(NULL, "view.3d", false);
    app_config__get_string(
        NULL, "tiles.dir", SETTINGS_DEFAULT_TILE_DIR, settings->tile_dir, sizeof(settings->tile_dir)
    );
    app_config__get_string(
        NULL, "tiles.url", SETTINGS_DEFAULT_TILE_URL, settings->tile_url, sizeof(settings->tile_url)
    );
    app_config__get_string(
        NULL, "search.url", SETTINGS_DEFAULT_SEARCH_URL, settings->search_url, sizeof(settings->search_url)
    );
    app_config__get_string(
        NULL, "route.url", SETTINGS_DEFAULT_ROUTE_URL, settings->route_url, sizeof(settings->route_url)
    );
}

void settings__save_view(const settings_t *settings) {
    (void)app_config__set_int(NULL, "view.lat_e6", settings->lat_e6);
    (void)app_config__set_int(NULL, "view.lon_e6", settings->lon_e6);
    (void)app_config__set_int(NULL, "view.zoom", settings->zoom);
}

void settings__save_online(bool online) { (void)app_config__set_bool(NULL, "tiles.online", online); }

void settings__save_big(bool big) { (void)app_config__set_bool(NULL, "view.big", big); }

void settings__save_3d(bool on) { (void)app_config__set_bool(NULL, "view.3d", on); }

size_t settings__load_history(search_place_t *places, size_t max) {
    char *lines[SETTINGS_HISTORY_MAX];
    for (size_t i = 0; i < SETTINGS_HISTORY_MAX; ++i) lines[i] = s_history[i];
    if (max > SETTINGS_HISTORY_MAX) max = SETTINGS_HISTORY_MAX;
    size_t read = app_config__get_string_array(NULL, "search.history", lines, sizeof(s_history[0]), max);
    size_t count = 0;
    for (size_t i = 0; i < read; ++i) {
        long lat = 0, lon = 0;
        int name = 0;
        if (sscanf(lines[i], "%ld %ld %n", &lat, &lon, &name) != 2 || lines[i][name] == '\0') continue;
        places[count].lat = lat / 1e6;
        places[count].lon = lon / 1e6;
        snprintf(places[count].name, sizeof(places[count].name), "%s", lines[i] + name);
        count++;
    }
    return count;
}

void settings__save_history(const search_place_t *places, size_t count) {
    const char *lines[SETTINGS_HISTORY_MAX];
    if (count > SETTINGS_HISTORY_MAX) count = SETTINGS_HISTORY_MAX;
    for (size_t i = 0; i < count; ++i) {
        snprintf(s_history[i], sizeof(s_history[i]), "%ld %ld %s", (long)floor(places[i].lat * 1e6 + 0.5),
                 (long)floor(places[i].lon * 1e6 + 0.5), places[i].name);
        lines[i] = s_history[i];
    }
    (void)app_config__set_string_array(NULL, "search.history", lines, count);
}
