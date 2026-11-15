#include "fakes.h"
#include "hud.h"
#include "test.h"

static void draw(double lat, double lon, int zoom, hud_state_t state, int width, int height) {
    map_view_t view;
    map_view__set(&view, lat, lon, zoom);
    hud__draw(&view, &state, width, height);
}

static void shows_centre_coordinates(void) {
    draw(50.0614, 19.9372, 18, (hud_state_t){0}, 240, 135);
    CHECK(fakes__find_prefix("50.0614") != NULL);
    CHECK(fakes__find_prefix("50.0614")->x > 0);
}

static void negative_coordinates_keep_sign(void) {
    draw(-33.8688, -70.6693, 18, (hud_state_t){0}, 240, 135);
    const fake_draw_t *text = fakes__find_prefix("-33.868");
    CHECK(text != NULL);
    if (text != NULL) CHECK(strstr(text->text, ", -70.669") != NULL);
}

static void shows_zoom_and_wheel_mode(void) {
    draw(0, 0, 7, (hud_state_t){0}, 240, 135);
    CHECK(fakes__find_string("z7") != NULL);
    fakes__reset();
    draw(0, 0, 7, (hud_state_t){.wheel_mode = HUD_WHEEL_PAN_X}, 240, 135);
    CHECK(fakes__find_string("z7 <>") != NULL);
    fakes__reset();
    draw(0, 0, 7, (hud_state_t){.wheel_mode = HUD_WHEEL_PAN_Y}, 240, 135);
    CHECK(fakes__find_string("z7 ^v") != NULL);
}

static void zoom_moves_under_coordinates_on_narrow_screen(void) {
    draw(-33.8688, -70.6693, 15, (hud_state_t){0}, 240, 135);
    int wide_y = fakes__find_string("z15")->y;
    fakes__reset();
    draw(-33.8688, -70.6693, 15, (hud_state_t){0}, 135, 240);
    CHECK(fakes__find_string("z15")->y > wide_y);
}

static void scale_bar_picks_round_distance(void) {
    draw(50.06, 19.94, 15, (hud_state_t){0}, 240, 135);
    CHECK(fakes__find_string("200 m") != NULL);
    fakes__reset();
    draw(50.06, 19.94, 5, (hud_state_t){0}, 240, 135);
    CHECK(fakes__find_string("200 km") != NULL);
}

static void shows_attribution_and_status(void) {
    draw(0, 0, 3, (hud_state_t){.status = "Loading"}, 240, 135);
    CHECK(fakes__find_string("(c) OpenStreetMap") != NULL);
    CHECK(fakes__find_string("Loading") != NULL);
    fakes__reset();
    draw(0, 0, 3, (hud_state_t){0}, 240, 135);
    CHECK(fakes__find_string("Loading") == NULL);
}

SUITE(hud_tests,
      TEST(shows_centre_coordinates),
      TEST(negative_coordinates_keep_sign),
      TEST(shows_zoom_and_wheel_mode),
      TEST(zoom_moves_under_coordinates_on_narrow_screen),
      TEST(scale_bar_picks_round_distance),
      TEST(shows_attribution_and_status))
