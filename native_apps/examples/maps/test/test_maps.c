/* The app's input and loop logic, compiled straight from maps.c. */
#include "../main/maps.c"

#include "fakes.h"
#include "route_fixture.h"
#include "test.h"

static maps_t app;

static void start(void) {
    memset(&app, 0, sizeof(app));
    settings__load(&app.settings);
    map_view__set(&app.view, 50.0, 20.0, 10);
    app.start_view = app.view;
    tiles__init(&app.tiles, 6, app.settings.tile_dir, app.settings.tile_url, false);
    app.running = true;
}

static void stop(void) { tiles__deinit(&app.tiles); }

static void press(bruce_input_type_t type, int32_t code) {
    bruce_input_event_t event = {.type = type, .action = BRUCE_INPUT_PRESS, .code = code};
    maps__handle(&app, &event);
}

static void arrows_pan_a_quarter_screen(void) {
    start();
    geo_point_t before = app.view.center;
    press(BRUCE_INPUT_KEY, BRUCE_INPUT_CODE_RIGHT);
    CHECK_EQ(app.view.center.x, before.x + 240 / 4);
    press(BRUCE_INPUT_KEY, BRUCE_INPUT_CODE_UP);
    CHECK_EQ(app.view.center.y, before.y - 135 / 4);
    CHECK(app.dirty);
    stop();
}

static void releases_are_ignored(void) {
    start();
    geo_point_t before = app.view.center;
    bruce_input_event_t event = {
        .type = BRUCE_INPUT_KEY,
        .action = BRUCE_INPUT_RELEASE,
        .code = BRUCE_INPUT_CODE_RIGHT,
    };
    maps__handle(&app, &event);
    CHECK_EQ(app.view.center.x, before.x);
    stop();
}

static void plus_minus_and_zoom_codes_zoom(void) {
    start();
    press(BRUCE_INPUT_KEY, '+');
    CHECK_EQ(app.view.zoom, 11);
    press(BRUCE_INPUT_KEY, '=');
    CHECK_EQ(app.view.zoom, 12);
    press(BRUCE_INPUT_KEY, '-');
    CHECK_EQ(app.view.zoom, 11);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_ZOOM_OUT);
    CHECK_EQ(app.view.zoom, 10);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_ZOOM_IN);
    CHECK_EQ(app.view.zoom, 11);
    stop();
}

static void back_exits(void) {
    start();
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_BACK);
    CHECK(!app.running);
    stop();
}

static void hold_select_switches_mode_with_hint(void) {
    start();
    fakes.long_press = true;
    fakes.now_ms = 1000;
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    CHECK_EQ(fakes.choice_calls, 0);
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_UP_DOWN);
    CHECK_STR(app.hud.hint[0], "Move up/down");
    CHECK(app.hud.hint[1] == NULL);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_LEFT_RIGHT);
    stop();
}

static void hint_disappears_after_two_seconds(void) {
    start();
    fakes.long_press = true;
    fakes.now_ms = 1000;
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    app.dirty = false;
    fakes.now_ms = 2999;
    maps__expire_hint(&app);
    CHECK(app.hud.hint[0] != NULL);
    CHECK(!app.dirty);
    fakes.now_ms = 3000;
    maps__expire_hint(&app);
    CHECK(app.hud.hint[0] == NULL);
    CHECK(app.dirty);
    stop();
}

static void held_enter_key_still_opens_menu(void) {
    start();
    fakes.long_press = true;
    press(BRUCE_INPUT_KEY, BRUCE_INPUT_CODE_SELECT);
    CHECK_EQ(fakes.choice_calls, 1);
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_ZOOM);
    stop();
}

static void select_enter_and_menu_open_menu(void) {
    start();
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    press(BRUCE_INPUT_KEY, BRUCE_INPUT_CODE_SELECT);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_MENU);
    CHECK_EQ(fakes.choice_calls, 3);
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_ZOOM);
    stop();
}

static void first_menu_item_cycles_nav_mode(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 0;
    maps__menu(&app);
    CHECK_STR(fakes.choice_labels[0], "Change to up/down");
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_UP_DOWN);
    CHECK_STR(app.hud.hint[0], "Move up/down");
    CHECK_STR(app.hud.hint[1], "Tip: hold Select");
    maps__menu(&app);
    CHECK_STR(fakes.choice_labels[0], "Change to left/right");
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_LEFT_RIGHT);
    maps__menu(&app);
    CHECK_STR(fakes.choice_labels[0], "Change to zoom");
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_ZOOM);
    stop();
}

static void prev_next_follow_nav_mode(void) {
    start();
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_NEXT);
    CHECK_EQ(app.view.zoom, 11);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_PREV);
    CHECK_EQ(app.view.zoom, 10);

    geo_point_t before = app.view.center;
    app.hud.nav_mode = HUD_NAV_UP_DOWN;
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_PREV);
    CHECK_EQ(app.view.center.y, before.y - 135 / 4);
    CHECK_EQ(app.view.center.x, before.x);

    app.hud.nav_mode = HUD_NAV_LEFT_RIGHT;
    press(BRUCE_INPUT_ENCODER, BRUCE_INPUT_CODE_NEXT);
    CHECK_EQ(app.view.center.x, before.x + 240 / 4);
    CHECK_EQ(app.view.zoom, 10);
    stop();
}

static void cancelled_menu_changes_nothing(void) {
    start();
    maps__menu(&app);
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_ZOOM);
    CHECK(app.running);
    stop();
}

static void touch_drag_moves_map_with_finger(void) {
    start();
    geo_point_t before = app.view.center;
    bruce_input_event_t touch[] = {
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_PRESS, .code = BRUCE_INPUT_CODE_TOUCH, .x = 100, .y = 50},
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_CHANGE, .code = BRUCE_INPUT_CODE_TOUCH, .x = 130, .y = 40},
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_CHANGE, .code = BRUCE_INPUT_CODE_TOUCH, .x = 140, .y = 40},
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_RELEASE, .code = BRUCE_INPUT_CODE_TOUCH, .x = 140, .y = 40},
    };
    for (size_t i = 0; i < sizeof(touch) / sizeof(touch[0]); ++i) maps__handle(&app, &touch[i]);
    CHECK_EQ(app.view.center.x, before.x - 40);
    CHECK_EQ(app.view.center.y, before.y + 10);
    stop();
}

static void menu_search_jumps_to_coordinates(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 1;
    fakes.text_input = "-33.8688, 151.2093";
    maps__menu(&app);
    double lat = 0, lon = 0;
    map_view__center(&app.view, &lat, &lon);
    CHECK_NEAR(lat, -33.8688, 0.0001);
    CHECK_NEAR(lon, 151.2093, 0.0001);
    CHECK_EQ(app.view.zoom, MAPS_PLACE_ZOOM);
    CHECK(app.hud.pin);
    CHECK_EQ(fakes.http_calls, 0);
    CHECK_EQ(fakes.message_count, 0);
    stop();
}

static void coordinates_keep_deep_zoom(void) {
    start();
    map_view__zoom(&app.view, 7);
    fakes.text_input = "40.7128 -74.006";
    maps__search(&app);
    double lat = 0, lon = 0;
    map_view__center(&app.view, &lat, &lon);
    CHECK_NEAR(lat, 40.7128, 0.0001);
    CHECK_EQ(app.view.zoom, 17);
    stop();
}

static const char *const s_two_places =
    "[{\"lat\":\"50.0547\",\"lon\":\"19.9354\",\"display_name\":\"Wawel, Kraków, Poland\"},"
    "{\"lat\":\"52.2297\",\"lon\":\"21.0122\",\"display_name\":\"Warsaw, Poland\"}]";

static void run_search(const char *query) {
    fakes.text_input = query;
    maps__search(&app);
    for (int i = 0; i < 10 && app.search.state != SEARCH_IDLE; ++i) maps__poll_search(&app);
}

static void search_lists_places_with_distance(void) {
    start();
    map_view__set(&app.view, 50.06, 19.94, 12);
    fakes.http_body = s_two_places;
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 1;
    run_search("wawel castle");
    CHECK(strstr(fakes.http_url, "q=wawel%20castle&format=jsonv2") != NULL);
    CHECK_STR(fakes.http_user_agent, "BruceOS-Maps/0.1");
    CHECK_EQ(fakes.choice_count, 2u);
    CHECK_STR(fakes.choice_labels[0], "Wawel, Kraków, Poland");
    CHECK(strstr(fakes.choice_right_text[0], " m") != NULL);
    CHECK_STR(fakes.choice_right_text[1], "252 km");
    double lat = 0, lon = 0;
    map_view__center(&app.view, &lat, &lon);
    CHECK_NEAR(lat, 52.2297, 0.0001);
    CHECK_EQ(app.view.zoom, MAPS_PLACE_ZOOM);
    CHECK(app.hud.pin);
    CHECK_STR(app.hud.place, "Warsaw, Poland");
    stop();
}

static void single_result_goes_straight_there(void) {
    start();
    fakes.http_body = "[{\"lat\":\"1.5\",\"lon\":\"2.5\",\"display_name\":\"Somewhere\"}]";
    run_search("somewhere");
    CHECK_EQ(fakes.choice_calls, 0);
    CHECK_NEAR(app.hud.pin_lat, 1.5, 1e-9);
    stop();
}

static void search_reports_nothing_found_and_errors(void) {
    start();
    fakes.http_body = "[]";
    run_search("nowhere");
    CHECK_STR(fakes.message, "No places found");
    fakes.http_status = 503;
    run_search("busy");
    CHECK_STR(fakes.message, "Search failed, try again later");
    CHECK(!app.hud.pin);
    stop();
}

static void search_connects_wifi_first(void) {
    start();
    fakes.wifi_connected = false;
    fakes.wifi_connect_manual = true;
    fakes.http_body = s_two_places;
    fakes.text_input = "wawel";
    maps__search(&app);
    maps__poll_search(&app);
    maps__update_status(&app, 0);
    CHECK_STR(app.hud.status, "Connecting");
    CHECK_EQ(fakes.http_calls, 0);
    fakes__wifi_finish();
    fakes.http_manual = true;
    maps__poll_search(&app);
    maps__update_status(&app, 3);
    CHECK_STR(app.hud.status, "Searching");
    CHECK_STR(app.hud.status_icon, "magnify");
    CHECK_EQ(fakes.http_calls, 1);
    stop();
}

static void search_offline_says_so(void) {
    start();
    fakes.wifi_connected = false;
    fakes.wifi_connect_ok = false;
    fakes.wifi_connect_manual = true;
    fakes.text_input = "wawel";
    maps__search(&app);
    fakes__wifi_finish();
    maps__poll_search(&app);
    CHECK_STR(fakes.message, "No internet connection");
    CHECK_EQ(app.search.state, SEARCH_IDLE);
    stop();
}

static void back_cancels_search_not_app(void) {
    start();
    fakes.http_manual = true;
    fakes.text_input = "wawel";
    maps__search(&app);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_BACK);
    CHECK(app.running);
    CHECK_EQ(app.search.state, SEARCH_IDLE);
    CHECK_EQ(fakes.http_cancels, 1);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_BACK);
    CHECK(!app.running);
    stop();
}

static void search_history_lists_found_places_latest_first(void) {
    start();
    fakes.text_input = "1, 2";
    maps__search(&app);
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 0; /* New search. */
    fakes.text_input = "3, 4";
    maps__search(&app);
    CHECK_EQ(fakes.choice_count, 2u);
    CHECK_STR(fakes.choice_labels[0], "New search");
    CHECK_STR(fakes.choice_labels[1], "1 2");
    CHECK_STR(fakes.choice_icons[1], "history");
    fakes.choice = 2; /* "1 2" again: no new search, and it moves to the front. */
    fakes.text_input = NULL;
    maps__search(&app);
    CHECK_NEAR(app.hud.pin_lat, 1, 1e-9);
    CHECK_STR(app.hud.place, "1 2");
    CHECK_EQ(app.history_count, 2u);
    search_place_t saved[SEARCH_RESULTS_MAX];
    CHECK_EQ(settings__load_history(saved, SEARCH_RESULTS_MAX), 2u);
    CHECK_STR(saved[0].name, "1 2");
    CHECK_STR(saved[1].name, "3 4");
    CHECK_NEAR(saved[1].lon, 4, 1e-6);
    stop();
}

static void search_history_keeps_the_latest_eight(void) {
    start();
    char text[16];
    for (int i = 0; i < SEARCH_RESULTS_MAX + 2; ++i) {
        fakes.choice_result = BRUCE_OK;
        fakes.choice = 0;
        snprintf(text, sizeof(text), "%d, 0", i);
        fakes.text_input = text;
        maps__search(&app);
    }
    CHECK_EQ(app.history_count, (size_t)SEARCH_RESULTS_MAX);
    CHECK_STR(app.history[0].name, "9 0");
    CHECK_STR(app.history[SEARCH_RESULTS_MAX - 1].name, "2 0");
    stop();
}

static void menu_offers_clear_pin_with_a_pin(void) {
    start();
    maps__show_place(&app, 50, 20, "Here");
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 3;
    maps__menu(&app);
    CHECK_EQ(fakes.choice_count, 8u);
    CHECK_STR(fakes.choice_labels[3], "Clear pin");
    CHECK(!app.hud.pin);
    CHECK(app.hud.place == NULL);
    maps__menu(&app);
    CHECK_EQ(fakes.choice_count, 7u);
    stop();
}

static void coordinates_name_the_pin(void) {
    start();
    fakes.text_input = " 50.0614,19.9372 ";
    maps__search(&app);
    CHECK_STR(app.hud.place, "50.0614 19.9372");
    stop();
}

static void menu_toggles_online_and_saves(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 3;
    maps__menu(&app);
    CHECK_STR(fakes.choice_right_text[3], "Off");
    CHECK(app.tiles.online);
    CHECK_STR(fakes__config_get("tiles.online"), "true");
    maps__menu(&app);
    CHECK_STR(fakes.choice_right_text[3], "On");
    CHECK(!app.tiles.online);
    stop();
}

static void menu_toggles_big_and_saves(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 4;
    maps__menu(&app);
    CHECK_STR(fakes.choice_labels[4], "Bigger text");
    CHECK_STR(fakes.choice_right_text[4], "Off");
    CHECK(app.view.big);
    CHECK_STR(fakes__config_get("view.big"), "true");
    maps__menu(&app);
    CHECK_STR(fakes.choice_right_text[4], "On");
    CHECK(!app.view.big);
    stop();
}

static void menu_hides_nav_mode_with_keyboard_or_touch(void) {
    start();
    fakes.has_keyboard = true;
    maps__menu(&app);
    CHECK_EQ(fakes.choice_count, 6u);
    CHECK_STR(fakes.choice_labels[0], "Search");
    fakes.has_keyboard = false;
    fakes.has_touch = true;
    maps__menu(&app);
    CHECK_EQ(fakes.choice_count, 6u);
    CHECK_STR(fakes.choice_labels[0], "Search");
    stop();
}

static void menu_exit_stops(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 5;
    maps__menu(&app);
    CHECK(!app.running);
    stop();
}

static void view_saved_only_when_moved(void) {
    start();
    maps__save_view(&app);
    CHECK_EQ(fakes.config_writes, 0);
    press(BRUCE_INPUT_KEY, '+');
    maps__save_view(&app);
    CHECK_STR(fakes__config_get("view.zoom"), "11");
    stop();
}

static void status_reflects_loading_and_network(void) {
    start();
    maps__update_status(&app, 2);
    CHECK_STR(app.hud.status, "Loading");
    CHECK(app.hud.status_icon == NULL);
    maps__update_status(&app, 0);
    CHECK(app.hud.status == NULL);
    app.tiles.network_down = true;
    maps__update_status(&app, 0);
    CHECK_STR(app.hud.status, "Offline");
    CHECK_STR(app.hud.status_icon, "wifi-cancel");
    app.tiles.connecting = true;
    maps__update_status(&app, 0);
    CHECK_STR(app.hud.status, "Connecting");
    CHECK_STR(app.hud.status_icon, "web");
    maps__update_status(&app, 3);
    CHECK_STR(app.hud.status, "Loading");
    stop();
}

static void menu_has_icons_and_back_last(void) {
    start();
    maps__menu(&app);
    CHECK_EQ(fakes.choice_count, 7u);
    CHECK_STR(fakes.choice_icons[0], "arrow-up-down");
    CHECK_STR(fakes.choice_labels[6], "Back");
    CHECK_STR(fakes.choice_icons[6], "arrow-left");
    CHECK_STR(fakes.choice_icons[4], "format-size");
    CHECK_STR(fakes.choice_icons[5], "power");
    app.hud.nav_mode = HUD_NAV_LEFT_RIGHT;
    maps__menu(&app);
    CHECK_STR(fakes.choice_icons[0], "plus");
    stop();
}

static void menu_back_just_closes(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 6;
    map_view_t before = app.view;
    maps__menu(&app);
    CHECK(app.running);
    CHECK_EQ(app.hud.nav_mode, HUD_NAV_ZOOM);
    CHECK_EQ(app.view.center.x, before.center.x);
    CHECK(app.tiles.online == false);
    stop();
}

static void touch(bruce_input_action_t action, int32_t x, int32_t y) {
    bruce_input_event_t event = {.type = BRUCE_INPUT_TOUCH, .action = action, .code = BRUCE_INPUT_CODE_TOUCH, .x = x, .y = y};
    maps__handle(&app, &event);
}

static void first_touch_shows_zoom_buttons(void) {
    start();
    CHECK(!app.hud.zoom_buttons);
    touch(BRUCE_INPUT_PRESS, 10, 10);
    CHECK(app.hud.zoom_buttons);
    CHECK(app.dirty);
    stop();
}

static void tapping_zoom_buttons_zooms(void) {
    start();
    hud_rect_t in = hud__button_rect(HUD_BUTTON_ZOOM_IN, 240, 135);
    hud_rect_t out = hud__button_rect(HUD_BUTTON_ZOOM_OUT, 240, 135);
    touch(BRUCE_INPUT_PRESS, in.x + 2, in.y + 2);
    touch(BRUCE_INPUT_RELEASE, in.x + 2, in.y + 2);
    CHECK_EQ(app.view.zoom, 11);
    touch(BRUCE_INPUT_PRESS, out.x + out.w - 1, out.y + out.h - 1);
    touch(BRUCE_INPUT_RELEASE, out.x + out.w - 1, out.y + out.h - 1);
    CHECK_EQ(app.view.zoom, 10);
    stop();
}

static void drag_from_button_does_not_pan(void) {
    start();
    hud_rect_t in = hud__button_rect(HUD_BUTTON_ZOOM_IN, 240, 135);
    touch(BRUCE_INPUT_PRESS, in.x + 2, in.y + 2);
    geo_point_t after_tap = app.view.center;
    touch(BRUCE_INPUT_CHANGE, in.x - 40, in.y + 2);
    CHECK_EQ(app.view.center.x, after_tap.x);
    touch(BRUCE_INPUT_RELEASE, in.x - 40, in.y + 2);

    touch(BRUCE_INPUT_PRESS, 50, 50);
    touch(BRUCE_INPUT_CHANGE, 60, 50);
    CHECK_EQ(app.view.center.x, after_tap.x - 10);
    stop();
}

static void gps_here(double lat, double lon) {
    fakes.gps_fix = (bruce_gps_fix_t){.receiving = true, .fix = true, .latitude = lat, .longitude = lon};
    fakes.now_ms += MAPS_GPS_POLL_MS;
    maps__poll_gps(&app);
}

static void gps_stays_off_until_locate(void) {
    start();
    app.gps_available = true;
    fakes.now_ms += MAPS_GPS_POLL_MS;
    maps__poll_gps(&app);
    CHECK_EQ(fakes.gps_reads, 0);
    CHECK(app.hud.gps_status == NULL);
    maps__locate(&app);
    CHECK(app.gps);
    maps__poll_gps(&app);
    CHECK_EQ(fakes.gps_reads, 1);
    stop();
}

static void gps_shows_position_speed_and_redraws_when_it_moves(void) {
    start();
    app.gps = true;
    fakes.gps_fix.receiving = true;
    maps__poll_gps(&app);
    CHECK(!app.hud.me);
    CHECK(!app.hud.speed);
    CHECK(app.hud.gps_status != NULL);
    app.dirty = false;
    gps_here(50.1, 20.1);
    CHECK(app.hud.me);
    CHECK(app.hud.speed);
    CHECK_EQ(app.hud.speed_kmh, 0);
    CHECK(app.dirty);
    app.dirty = false;
    gps_here(50.1, 20.1);
    CHECK(!app.dirty);
    fakes.gps_fix.speed_kmh = 41.6f;
    fakes.now_ms += MAPS_GPS_POLL_MS;
    maps__poll_gps(&app);
    CHECK_EQ(app.hud.speed_kmh, 42);
    CHECK(app.dirty);
    int reads = fakes.gps_reads;
    maps__poll_gps(&app);
    CHECK_EQ(fakes.gps_reads, reads);
    stop();
}

static void locate_centres_on_first_fix_and_follows_until_panned(void) {
    start();
    app.gps_available = true;
    maps__locate(&app);
    CHECK(!app.hud.following);
    CHECK(app.locate_pending);
    gps_here(50.1, 20.1);
    CHECK(app.hud.following);
    CHECK(!app.locate_pending);
    CHECK_EQ(app.view.zoom, MAPS_PLACE_ZOOM);
    geo_point_t here = geo__from_lat_lon(50.1, 20.1, MAPS_PLACE_ZOOM);
    CHECK_EQ(app.view.center.x, here.x);
    gps_here(50.2, 20.2);
    CHECK_EQ(app.view.center.x, geo__from_lat_lon(50.2, 20.2, MAPS_PLACE_ZOOM).x);
    press(BRUCE_INPUT_KEY, BRUCE_INPUT_CODE_RIGHT);
    CHECK(!app.hud.following);
    geo_point_t panned = app.view.center;
    gps_here(50.3, 20.3);
    CHECK_EQ(app.view.center.x, panned.x);
    maps__locate(&app);
    CHECK(app.hud.following);
    stop();
}

/* Places the GPS position (dx, dy) screen pixels from the map centre. */
static void gps_at_offset(int32_t dx, int32_t dy) {
    geo_point_t point = {app.view.center.x + dx, app.view.center.y + dy};
    double lat = 0, lon = 0;
    geo__to_lat_lon(point, app.view.zoom, &lat, &lon);
    gps_here(lat, lon);
}

static void epaper_map_moves_only_when_position_leaves_middle(void) {
    start();
    fakes.width = 240;
    fakes.height = 320;
    fakes.epaper = true;
    app.gps_available = true;
    map_view__set(&app.view, 50.1, 20.1, MAPS_PLACE_ZOOM);
    maps__locate(&app);
    gps_here(50.1, 20.1);
    geo_point_t centre = app.view.center;
    gps_at_offset(50, -70);
    CHECK_EQ(app.view.center.x, centre.x);
    CHECK_EQ(app.view.center.y, centre.y);
    gps_at_offset(70, 0);
    geo_point_t me = geo__from_lat_lon(app.hud.me_lat, app.hud.me_lon, app.view.zoom);
    CHECK(abs(me.x - app.view.center.x + 60) <= 1);
    CHECK_EQ(app.view.center.y, centre.y);
    gps_at_offset(0, 1000);
    me = geo__from_lat_lon(app.hud.me_lat, app.hud.me_lon, app.view.zoom);
    CHECK_EQ(app.view.center.y, me.y);
    fakes.epaper = false;
    gps_at_offset(10, 0);
    me = geo__from_lat_lon(app.hud.me_lat, app.hud.me_lon, app.view.zoom);
    CHECK_EQ(app.view.center.x, me.x);
    stop();
}

static void tapping_locate_button_and_menu_locate(void) {
    start();
    app.gps_available = app.hud.locate_button = true;
    hud_rect_t button = hud__button_rect(HUD_BUTTON_LOCATE, 240, 135);
    touch(BRUCE_INPUT_PRESS, button.x + 2, button.y + 2);
    CHECK(app.gps);
    touch(BRUCE_INPUT_CHANGE, button.x - 40, button.y + 2);
    touch(BRUCE_INPUT_RELEASE, button.x - 40, button.y + 2);
    gps_here(50.1, 20.1);
    CHECK(app.hud.following);

    app.hud.following = false;
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 3;
    maps__menu(&app);
    CHECK_STR(fakes.choice_labels[3], "My location");
    CHECK(app.hud.following);
    stop();
}

static void menu_turns_gps_off(void) {
    start();
    app.gps_available = true;
    maps__menu(&app);
    CHECK_EQ(fakes.choice_count, 8u);
    maps__locate(&app);
    gps_here(50.1, 20.1);
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 4;
    maps__menu(&app);
    CHECK_STR(fakes.choice_labels[4], "Turn GPS off");
    CHECK(!app.gps && !app.hud.me && !app.hud.speed && !app.hud.following);
    int reads = fakes.gps_reads;
    gps_here(50.1, 20.1);
    CHECK_EQ(fakes.gps_reads, reads);
    stop();
}

static void gps_status_shows_progress_until_fix(void) {
    start();
    app.gps = true;
    fakes.now_ms = 1000;
    maps__poll_gps(&app);
    CHECK_STR(app.hud.gps_status, "GPS 1/3: starting");
    fakes.now_ms += MAPS_GPS_NO_DATA_MS;
    maps__poll_gps(&app);
    CHECK_STR(app.hud.gps_status, "GPS 1/3: no data");
    fakes.gps_fix = (bruce_gps_fix_t){.receiving = true, .satellites_in_view = 7};
    fakes.now_ms += MAPS_GPS_POLL_MS;
    maps__poll_gps(&app);
    CHECK_STR(app.hud.gps_status, "GPS 2/3: time sync, 7 seen");
    fakes.gps_fix.time_valid = true;
    fakes.gps_fix.satellites = 2;
    fakes.now_ms += MAPS_GPS_POLL_MS;
    app.dirty = false;
    maps__poll_gps(&app);
    CHECK_STR(app.hud.gps_status, "GPS 3/3: 2/4 locked, 7 seen");
    CHECK(app.dirty);
    app.dirty = false;
    fakes.now_ms += MAPS_GPS_POLL_MS;
    maps__poll_gps(&app);
    CHECK(!app.dirty);
    gps_here(50.1, 20.1);
    CHECK(app.hud.gps_status == NULL);
    stop();
}

static void gps_error_hides_location(void) {
    start();
    app.gps_available = app.hud.locate_button = true;
    maps__locate(&app);
    gps_here(50.1, 20.1);
    fakes.gps_result = BRUCE_ERR_PERMISSION;
    gps_here(50.1, 20.1);
    CHECK(!app.gps);
    CHECK(!app.hud.me);
    CHECK(!app.hud.locate_button);
    CHECK(app.hud.gps_status == NULL);
    maps__menu(&app);
    CHECK_EQ(fakes.choice_count, 7u);
    stop();
}

static void finish_route(void) {
    for (int i = 0; i < 10 && app.route_request.state != ROUTE_IDLE; ++i) maps__poll_route(&app);
}

static void directions_preview_steps_through_the_route(void) {
    start();
    fakes.width = 240;
    fakes.height = 320;
    maps__show_place(&app, 50.063, 19.935, "North end");
    map_view__set(&app.view, 50.06, 19.93, 14);
    fakes.http_body = s_route_json;
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 0; /* The pin, from the map centre, by car. */
    maps__directions(&app);
    finish_route();
    CHECK(strstr(fakes.http_url, "/routed-car/route/v1/driving/19.9") != NULL);
    CHECK(strstr(fakes.http_url, ";19.93500,50.06300?") != NULL);
    CHECK_EQ(app.route.step_count, 3u);
    CHECK(!app.navigating);
    CHECK(app.hud.route == &app.route);
    CHECK(app.hud.banner_step == &app.route.steps[0]);
    CHECK_STR(app.hud.banner_distance, "1/3");
    CHECK_STR(app.hud.banner_text, "Head east on Main");
    CHECK_STR(app.hud.banner_then, "Turn left onto N1");
    CHECK(strstr(app.hud.summary, " min") != NULL);
    CHECK_STR(app.hud.place, "North end");
    hud_rect_t banner = hud__banner_rect(240);
    touch(BRUCE_INPUT_PRESS, banner.x + 10, banner.y + 10);
    CHECK_STR(app.hud.banner_text, "Turn left onto N1");
    CHECK_STR(app.hud.banner_then, "Arrive at the destination");
    press(BRUCE_INPUT_KEY, BRUCE_INPUT_CODE_BACK);
    CHECK(app.running);
    CHECK_EQ(app.route.point_count, 0u);
    CHECK(app.hud.banner_step == NULL);
    CHECK(app.hud.route == NULL);
    stop();
}

static void directions_navigate_reroute_and_arrive(void) {
    start();
    fakes.width = 240;
    fakes.height = 320;
    app.gps_available = true;
    maps__show_place(&app, 50.063, 19.935, "North end");
    fakes.http_body = s_route_json;
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 0; /* The pin, from my location, by car. */
    maps__directions(&app);
    CHECK(app.route_wait_fix);
    CHECK(app.gps);
    CHECK_EQ(fakes.http_calls, 0);
    gps_here(50.06, 19.93);
    finish_route();
    CHECK(app.navigating);
    CHECK(app.hud.following);
    CHECK_STR(app.hud.banner_distance, "350 m");
    CHECK_STR(app.hud.banner_text, "Turn left onto N1");
    CHECK_STR(app.hud.banner_then, "Arrive at the destination");
    gps_here(50.06, 19.9325);
    CHECK_STR(app.hud.banner_distance, "200 m");
    int calls = fakes.http_calls;
    for (int i = 0; i < MAPS_OFF_ROUTE_FIXES; ++i) gps_here(50.07, 19.95);
    CHECK_EQ(fakes.http_calls, calls + 1);
    CHECK(strstr(fakes.http_url, "19.95000,50.07000;19.93500,50.06300") != NULL);
    finish_route();
    CHECK(app.navigating);
    gps_here(50.063, 19.935);
    CHECK(!app.navigating);
    CHECK_EQ(app.route.point_count, 0u);
    CHECK_STR(app.hud.hint[0], "You have arrived");
    stop();
}

static void directions_report_no_route(void) {
    start();
    maps__show_place(&app, 50.063, 19.935, "North end");
    fakes.http_status = 400;
    fakes.http_body = "{\"code\":\"NoRoute\",\"message\":\"Impossible route\"}";
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 0;
    maps__directions(&app);
    finish_route();
    CHECK_STR(fakes.message, "No route found");
    CHECK_EQ(app.route.point_count, 0u);
    stop();
}

static void app_main_draws_loads_and_exits(void) {
    fakes__add_file("/sdcard/maps/tiles/4/8/5.png", 500);
    fakes.polls_until_back = 8;
    CHECK_EQ(app_main(0, NULL), 0);
    CHECK(fakes.frames >= 2);
    CHECK(fakes.decode_count >= 1);
    CHECK(fakes__count(FAKE_RGB_BITMAP) >= 1);
    CHECK_EQ(fakes.config_writes, 0);
}

static void no_wifi_status_clears_when_wifi_comes_back(void) {
    start();
    tiles__set_online(&app.tiles, true);
    app.tiles.network_down = true;
    fakes.wifi_connected = true;
    fakes.now_ms = 5000;
    fakes.polls_until_back = 3;
    maps__run(&app);
    CHECK(!app.tiles.network_down);
    CHECK(app.hud.status == NULL || strcmp(app.hud.status, "No Wi-Fi") != 0);
    stop();
}

static void scrolling_keeps_five_downloads_and_cancels_far_ones(void) {
    start();
    fakes.width = 240;
    fakes.height = 320;
    fakes.http_manual = true;
    tiles__set_online(&app.tiles, true);
    app.view.center = (geo_point_t){10 * GEO_TILE_SIZE, 12 * GEO_TILE_SIZE + 128}; /* 2x3 tiles visible */
    fakes.polls_until_back = 10;
    maps__run(&app);
    CHECK_EQ(fakes.http_calls, 5);
    CHECK_EQ(app.tiles.download_count, 5u);
    CHECK_STR(app.hud.status, "Loading");

    map_view__pan(&app.view, 20 * GEO_TILE_SIZE, 0);
    app.running = true;
    fakes.polls_until_back = 10;
    maps__run(&app);
    CHECK_EQ(fakes.http_cancels, 5);
    CHECK_EQ(fakes.http_calls, 10);
    CHECK(fakes.http_max_active <= TILES_DOWNLOADS_MAX);

    fakes__http_finish_all();
    app.running = true;
    fakes.polls_until_back = 20;
    maps__run(&app);
    CHECK(fakes.http_max_active <= TILES_DOWNLOADS_MAX);
    CHECK(fakes.decode_count >= 5);
    stop();
}

static void stays_responsive_while_downloading(void) {
    start();
    fakes.http_manual = true;
    tiles__set_online(&app.tiles, true);
    fakes__push(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_RIGHT, 0);
    fakes.polls_until_back = 6;
    geo_point_t before = app.view.center;
    maps__run(&app);
    CHECK_EQ(app.view.center.x, before.x + 240 / 4);
    CHECK(fakes.http_calls > 0);
    CHECK_EQ(fakes.decode_count, 0);
    stop();
}

static void map_stays_responsive_while_wifi_connects(void) {
    start();
    fakes.wifi_connected = false;
    fakes.wifi_connect_manual = true;
    tiles__set_online(&app.tiles, true);
    fakes__push(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_RIGHT, 0);
    fakes.polls_until_back = 6;
    geo_point_t before = app.view.center;
    maps__run(&app);
    CHECK_EQ(app.view.center.x, before.x + 240 / 4);
    CHECK_EQ(fakes.wifi_connect_calls, 1);
    CHECK_STR(app.hud.status, "Connecting");

    fakes__wifi_finish();
    app.running = true;
    fakes.polls_until_back = 6;
    maps__run(&app);
    CHECK(fakes.http_calls > 0);
    stop();
}

static void tapping_search_box_opens_search(void) {
    start();
    fakes.text_input = "1, 2";
    hud_rect_t box = hud__search_rect(&app.view, &app.hud, 240);
    bruce_input_event_t tap = {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_PRESS,
                               .code = BRUCE_INPUT_CODE_TOUCH, .x = box.x + box.w / 2, .y = box.y + 2};
    maps__handle(&app, &tap);
    CHECK_STR(app.hud.place, "1 2");
    stop();
}

SUITE(maps_tests,
      TEST(arrows_pan_a_quarter_screen),
      TEST(releases_are_ignored),
      TEST(plus_minus_and_zoom_codes_zoom),
      TEST(back_exits),
      TEST(hold_select_switches_mode_with_hint),
      TEST(hint_disappears_after_two_seconds),
      TEST(held_enter_key_still_opens_menu),
      TEST(select_enter_and_menu_open_menu),
      TEST(first_menu_item_cycles_nav_mode),
      TEST(prev_next_follow_nav_mode),
      TEST(cancelled_menu_changes_nothing),
      TEST(touch_drag_moves_map_with_finger),
      TEST(menu_search_jumps_to_coordinates),
      TEST(coordinates_keep_deep_zoom),
      TEST(search_lists_places_with_distance),
      TEST(single_result_goes_straight_there),
      TEST(search_reports_nothing_found_and_errors),
      TEST(search_connects_wifi_first),
      TEST(search_offline_says_so),
      TEST(back_cancels_search_not_app),
      TEST(menu_offers_clear_pin_with_a_pin),
      TEST(coordinates_name_the_pin),
      TEST(search_history_lists_found_places_latest_first),
      TEST(search_history_keeps_the_latest_eight),
      TEST(tapping_search_box_opens_search),
      TEST(menu_toggles_online_and_saves),
      TEST(menu_toggles_big_and_saves),
      TEST(menu_hides_nav_mode_with_keyboard_or_touch),
      TEST(menu_exit_stops),
      TEST(view_saved_only_when_moved),
      TEST(status_reflects_loading_and_network),
      TEST(menu_has_icons_and_back_last),
      TEST(menu_back_just_closes),
      TEST(first_touch_shows_zoom_buttons),
      TEST(tapping_zoom_buttons_zooms),
      TEST(drag_from_button_does_not_pan),
      TEST(gps_stays_off_until_locate),
      TEST(gps_shows_position_speed_and_redraws_when_it_moves),
      TEST(locate_centres_on_first_fix_and_follows_until_panned),
      TEST(epaper_map_moves_only_when_position_leaves_middle),
      TEST(tapping_locate_button_and_menu_locate),
      TEST(menu_turns_gps_off),
      TEST(gps_status_shows_progress_until_fix),
      TEST(gps_error_hides_location),
      TEST(directions_preview_steps_through_the_route),
      TEST(directions_navigate_reroute_and_arrive),
      TEST(directions_report_no_route),
      TEST(app_main_draws_loads_and_exits),
      TEST(no_wifi_status_clears_when_wifi_comes_back),
      TEST(scrolling_keeps_five_downloads_and_cancels_far_ones),
      TEST(stays_responsive_while_downloading),
      TEST(map_stays_responsive_while_wifi_connects))
