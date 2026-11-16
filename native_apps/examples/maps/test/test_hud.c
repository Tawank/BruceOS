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
    CHECK_EQ(fakes__count(FAKE_ICON), 1u); /* The search box's magnify. */

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
        CHECK(plus->x > in.x && plus->x + plus->w < in.x + in.w);
        CHECK(minus->y > plus->y);
        CHECK(in.x + in.w <= 240 && out.y + out.h <= 135);
        CHECK(in.y + in.h <= out.y);
    }
}

static void status_and_hint_show_icons(void) {
    hud_state_t state = {.status = "No Wi-Fi", .status_icon = "wifi-cancel", .nav_mode = HUD_NAV_UP_DOWN};
    state.hint[0] = "Move up/down";
    state.hint_icon = "arrow-up-down";
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

static void search_box_fills_top_row_before_zoom(void) {
    map_view_t view;
    map_view__set(&view, 0, 0, 15);
    hud_state_t state = {0};
    hud__draw(&view, &state, 135, 240);
    const fake_draw_t *placeholder = fakes__find_string("Search");
    const fake_draw_t *zoom = fakes__find_string("z15");
    const fake_draw_t *coordinates = fakes__find_prefix("0.00000");
    CHECK(placeholder != NULL && zoom != NULL && coordinates != NULL);
    if (placeholder == NULL || zoom == NULL || coordinates == NULL) return;
    CHECK_EQ(placeholder->y, zoom->y);
    CHECK(coordinates->y > zoom->y);
    hud_rect_t box = hud__search_rect(&view, &state, 135);
    CHECK_EQ(box.x, 4);
    CHECK(box.x + box.w < zoom->x);
    CHECK(fakes__find_icon("magnify") != NULL);
}

static void search_box_shows_place_name_cut_to_fit(void) {
    draw(0, 0, 15, (hud_state_t){.pin = true, .place = "Wawel, Kraków, Poland"}, 240, 135);
    CHECK(fakes__find_string("Wawel") != NULL);
    CHECK(fakes__find_string("Search") == NULL);
    fakes__reset();
    draw(0, 0, 15, (hud_state_t){.place = "Muzeum Narodowe w Krakowie Gmach Główny Kraków"}, 135, 240);
    const fake_draw_t *name = fakes__find_prefix("Muzeum");
    CHECK(name != NULL);
    if (name == NULL) return;
    size_t len = strlen(name->text);
    CHECK(len > 3 && strcmp(name->text + len - 3, "...") == 0);
    hud_rect_t box = hud__search_rect(&(map_view_t){.zoom = 15}, &(hud_state_t){0}, 135);
    CHECK(name->x + (int)(len * 6) <= box.x + box.w);
    fakes__reset();
    draw(0, 0, 15, (hud_state_t){.place = "Zażółć gęślą jaźń łódź"}, 135, 240);
    name = fakes__find_prefix("Zażółć");
    CHECK(name != NULL);
    if (name != NULL) CHECK_STR(name->text, "Zażółć gę...");
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
    draw(0, 0, 3, (hud_state_t){.hint = {"Zoom in/out", "Tip: hold Select"}, .hint_icon = "plus"}, 240, 135);
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

static void pin_tip_marks_the_place(void) {
    map_view_t view;
    map_view__set(&view, 50.0, 20.0, 12);
    hud_state_t state = {.pin = true, .pin_lat = 50.0, .pin_lon = 20.0};
    fakes.draw_count = 0;
    hud__draw(&view, &state, 240, 135);
    const fake_draw_t *pin = NULL;
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        const fake_draw_t *d = &fakes.draws[i];
        if (d->kind == FAKE_ICON && strcmp(d->text, "map-marker") == 0) pin = d;
    }
    CHECK(pin != NULL);
    if (pin != NULL) {
        CHECK_EQ(pin->x + pin->w / 2, 120);
        CHECK_EQ(pin->y + pin->h - 2, 135 / 2);
    }
    state.pin_lat = -10.0;
    fakes.draw_count = 0;
    hud__draw(&view, &state, 240, 135);
    CHECK(fakes__find_icon("map-marker") == NULL);
}

SUITE(hud_tests,
      TEST(shows_centre_coordinates),
      TEST(negative_coordinates_keep_sign),
      TEST(shows_zoom_and_nav_mode_icon),
      TEST(zoom_buttons_only_when_enabled),
      TEST(status_and_hint_show_icons),
      TEST(search_box_fills_top_row_before_zoom),
      TEST(search_box_shows_place_name_cut_to_fit),
      TEST(scale_bar_picks_round_distance),
      TEST(shows_attribution_and_status),
      TEST(hint_is_centred_on_bottom_edge),
      TEST(pin_tip_marks_the_place))
