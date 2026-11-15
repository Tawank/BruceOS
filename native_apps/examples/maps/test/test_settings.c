#include "fakes.h"
#include "settings.h"
#include "test.h"

static void defaults_without_config(void) {
    settings_t settings;
    settings__load(&settings);
    CHECK_EQ(settings.lat_e6, 50000000);
    CHECK_EQ(settings.lon_e6, 15000000);
    CHECK_EQ(settings.zoom, 4);
    CHECK(settings.online);
    CHECK_STR(settings.tile_dir, "/sdcard/maps/tiles");
    CHECK_STR(settings.tile_url, "https://tile.openstreetmap.org/{z}/{x}/{y}.png");
}

static void stored_values_win(void) {
    fakes__config_set("view.lat_e6", "-33868800");
    fakes__config_set("view.zoom", "12");
    fakes__config_set("tiles.online", "false");
    fakes__config_set("tiles.url", "http://local/{z}/{x}/{y}.png");
    settings_t settings;
    settings__load(&settings);
    CHECK_EQ(settings.lat_e6, -33868800);
    CHECK_EQ(settings.zoom, 12);
    CHECK(!settings.online);
    CHECK_STR(settings.tile_url, "http://local/{z}/{x}/{y}.png");
}

static void invalid_zoom_falls_back(void) {
    fakes__config_set("view.zoom", "25");
    settings_t settings;
    settings__load(&settings);
    CHECK_EQ(settings.zoom, 4);
}

static void saves_view_and_online(void) {
    settings_t settings = {.lat_e6 = 1, .lon_e6 = -2, .zoom = 9};
    settings__save_view(&settings);
    settings__save_online(false);
    CHECK_STR(fakes__config_get("view.lat_e6"), "1");
    CHECK_STR(fakes__config_get("view.lon_e6"), "-2");
    CHECK_STR(fakes__config_get("view.zoom"), "9");
    CHECK_STR(fakes__config_get("tiles.online"), "false");
}

SUITE(settings_tests,
      TEST(defaults_without_config),
      TEST(stored_values_win),
      TEST(invalid_zoom_falls_back),
      TEST(saves_view_and_online))
