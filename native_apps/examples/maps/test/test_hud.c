#include <stdlib.h>

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

static void shows_zoom_and_nav_mode_icon(void) {
    draw(0, 0, 7, (hud_state_t){0}, 240, 135);
    CHECK(fakes__find_string("z7") != NULL);
    CHECK_EQ(fakes__count(FAKE_ICON), 0u);

    fakes__reset();
    draw(0, 0, 7, (hud_state_t){.nav_mode = HUD_NAV_UP_DOWN}, 240, 135);
    const fake_draw_t *icon = fakes__find_icon("arrow-up-down");
    const fake_draw_t *text = fakes__find_string("z7");
    CHECK(icon != NULL && text != NULL);
    if (icon != NULL && text != NULL) CHECK(icon->x < text->x);

    fakes__reset();
    draw(0, 0, 7, (hud_state_t){.nav_mode = HUD_NAV_LEFT_RIGHT}, 240, 135);
    CHECK(fakes__find_icon("arrow-left-right") != NULL);
}

static void zoom_buttons_only_when_enabled(void) {
    draw(0, 0, 7, (hud_state_t){0}, 240, 135);
    CHECK(fakes__find_icon("plus") == NULL);
    fakes__reset();
    draw(0, 0, 7, (hud_state_t){.zoom_buttons = true}, 240, 135);
    const fake_draw_t *plus = fakes__find_icon("plus");
    const fake_draw_t *minus = fakes__find_icon("minus");
    CHECK(plus != NULL && minus != NULL);
    if (plus != NULL && minus != NULL) {
        hud_rect_t in = hud__button_rect(HUD_BUTTON_ZOOM_IN, 240, 135);
        hud_rect_t out = hud__button_rect(HUD_BUTTON_ZOOM_OUT, 240, 135);
        CHECK(plus->x > in.x && plus->x + plus->w < in.x + in.size);
        CHECK(minus->y > plus->y);
        CHECK(in.x + in.size <= 240 && out.y + out.size <= 135);
        CHECK(in.y + in.size <= out.y);
    }
}

static void status_and_hint_show_icons(void) {
    hud_state_t state = {.status = "No Wi-Fi", .status_icon = "wifi-cancel", .nav_mode = HUD_NAV_UP_DOWN};
    state.hint[0] = "Move up/down";
    draw(0, 0, 7, state, 240, 135);
    CHECK(fakes__find_icon("wifi-cancel") != NULL);
    const fake_draw_t *hint = fakes__find_string("Move up/down");
    bool hint_icon = false;
    for (size_t i = 0; hint != NULL && i < fakes.draw_count; ++i) {
        const fake_draw_t *d = &fakes.draws[i];
        hint_icon |= d->kind == FAKE_ICON && strcmp(d->text, "arrow-up-down") == 0 && d->y > 135 / 2;
    }
    CHECK(hint_icon);
    if (hint != NULL) CHECK_EQ(hint->y + 8 + 4, 135 - 4);
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

static void hint_is_centred_on_bottom_edge(void) {
    draw(0, 0, 3, (hud_state_t){.hint = {"Zoom in/out", "Tip: hold Select"}}, 240, 135);
    const fake_draw_t *first = fakes__find_string("Zoom in/out");
    const fake_draw_t *second = fakes__find_string("Tip: hold Select");
    CHECK(first != NULL && second != NULL);
    if (first != NULL && second != NULL) {
        CHECK(second->y > first->y);
        CHECK_EQ(second->y + 8 + 4, 135 - 4);
        int pill = (int)strlen("Zoom in/out") * 6 + 8 + 12 + 3;
        int left = first->x - 4 - 12 - 3;
        CHECK(abs(left + pill / 2 - 120) <= 1);
    }
}

SUITE(hud_tests,
      TEST(shows_centre_coordinates),
      TEST(negative_coordinates_keep_sign),
      TEST(shows_zoom_and_nav_mode_icon),
      TEST(zoom_buttons_only_when_enabled),
      TEST(status_and_hint_show_icons),
      TEST(zoom_moves_under_coordinates_on_narrow_screen),
      TEST(scale_bar_picks_round_distance),
      TEST(shows_attribution_and_status),
      TEST(hint_is_centred_on_bottom_edge))
