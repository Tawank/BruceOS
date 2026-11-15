#include "settings.h"

#include "core_sdk/app_config.h"
#include "geo.h"

#define SETTINGS_DEFAULT_TILE_DIR "/sdcard/maps/tiles"
#define SETTINGS_DEFAULT_TILE_URL "https://tile.openstreetmap.org/{z}/{x}/{y}.png"

void settings__load(settings_t *settings) {
    settings->lat_e6 = app_config__get_int(NULL, "view.lat_e6", 50000000);
    settings->lon_e6 = app_config__get_int(NULL, "view.lon_e6", 15000000);
    settings->zoom = app_config__get_int(NULL, "view.zoom", 4);
    if (settings->zoom < GEO_MIN_ZOOM || settings->zoom > GEO_MAX_ZOOM) settings->zoom = 4;
    settings->online = app_config__get_bool(NULL, "tiles.online", true);
    app_config__get_string(
        NULL, "tiles.dir", SETTINGS_DEFAULT_TILE_DIR, settings->tile_dir, sizeof(settings->tile_dir)
    );
    app_config__get_string(
        NULL, "tiles.url", SETTINGS_DEFAULT_TILE_URL, settings->tile_url, sizeof(settings->tile_url)
    );
}

void settings__save_view(const settings_t *settings) {
    (void)app_config__set_int(NULL, "view.lat_e6", settings->lat_e6);
    (void)app_config__set_int(NULL, "view.lon_e6", settings->lon_e6);
    (void)app_config__set_int(NULL, "view.zoom", settings->zoom);
}

void settings__save_online(bool online) { (void)app_config__set_bool(NULL, "tiles.online", online); }
