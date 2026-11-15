/* The app's input and loop logic, compiled straight from maps.c. */
#include "../main/maps.c"

#include "fakes.h"
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

static void encoder_click_cycles_wheel(void) {
    start();
    press(BRUCE_INPUT_ENCODER, BRUCE_INPUT_CODE_NEXT);
    CHECK_EQ(app.view.zoom, 11);

    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    CHECK_EQ(app.hud.wheel_mode, HUD_WHEEL_PAN_X);
    geo_point_t before = app.view.center;
    press(BRUCE_INPUT_ENCODER, BRUCE_INPUT_CODE_PREV);
    CHECK_EQ(app.view.center.x, before.x - 240 / 8);

    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    CHECK_EQ(app.hud.wheel_mode, HUD_WHEEL_PAN_Y);
    press(BRUCE_INPUT_ENCODER, BRUCE_INPUT_CODE_NEXT);
    CHECK_EQ(app.view.center.y, before.y + 135 / 8);

    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    CHECK_EQ(app.hud.wheel_mode, HUD_WHEEL_ZOOM);
    CHECK_EQ(fakes.choice_calls, 0);
    stop();
}

static void long_press_enter_and_menu_open_menu(void) {
    start();
    fakes.long_press = true;
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_SELECT);
    CHECK_EQ(fakes.choice_calls, 1);
    CHECK_EQ(app.hud.wheel_mode, HUD_WHEEL_ZOOM);

    fakes.long_press = false;
    press(BRUCE_INPUT_KEY, BRUCE_INPUT_CODE_SELECT);
    press(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CODE_MENU);
    CHECK_EQ(fakes.choice_calls, 3);
    stop();
}

static void touch_drag_moves_map_with_finger(void) {
    start();
    geo_point_t before = app.view.center;
    bruce_input_event_t touch[] = {
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_PRESS, .code = 100, .value = 50},
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_CHANGE, .code = 130, .value = 40},
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_CHANGE, .code = 140, .value = 40},
        {.type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_RELEASE, .code = 140, .value = 40},
    };
    for (size_t i = 0; i < sizeof(touch) / sizeof(touch[0]); ++i) maps__handle(&app, &touch[i]);
    CHECK_EQ(app.view.center.x, before.x - 40);
    CHECK_EQ(app.view.center.y, before.y + 10);
    stop();
}

static void menu_go_to_coordinates(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 0;
    fakes.text_input = "-33.8688, 151.2093";
    maps__menu(&app);
    double lat = 0, lon = 0;
    map_view__center(&app.view, &lat, &lon);
    CHECK_NEAR(lat, -33.8688, 0.0001);
    CHECK_NEAR(lon, 151.2093, 0.0001);
    CHECK_EQ(app.view.zoom, MAPS_GOTO_ZOOM);
    CHECK_EQ(fakes.message_count, 0);
    stop();
}

static void go_to_accepts_space_separator_and_keeps_deep_zoom(void) {
    start();
    map_view__zoom(&app.view, 7);
    fakes.text_input = "40.7128 -74.006";
    maps__go_to(&app);
    double lat = 0, lon = 0;
    map_view__center(&app.view, &lat, &lon);
    CHECK_NEAR(lat, 40.7128, 0.0001);
    CHECK_EQ(app.view.zoom, 17);
    stop();
}

static void go_to_rejects_bad_input(void) {
    const char *inputs[] = {"abc", "50.1", "91, 10", "10, 181"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        start();
        geo_point_t before = app.view.center;
        fakes.text_input = inputs[i];
        maps__go_to(&app);
        CHECK_EQ(app.view.center.x, before.x);
        CHECK_EQ(fakes.message_count, (int)i + 1);
        stop();
    }
}

static void menu_toggles_online_and_saves(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 1;
    maps__menu(&app);
    CHECK_STR(fakes.choice_right_text[1], "Off");
    CHECK(app.tiles.online);
    CHECK_STR(fakes__config_get("tiles.online"), "true");
    maps__menu(&app);
    CHECK_STR(fakes.choice_right_text[1], "On");
    CHECK(!app.tiles.online);
    stop();
}

static void menu_exit_stops(void) {
    start();
    fakes.choice_result = BRUCE_OK;
    fakes.choice = 4;
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
    CHECK_STR(maps__status(&app, 2), "Loading");
    CHECK(maps__status(&app, 0) == NULL);
    app.tiles.network_down = true;
    CHECK_STR(maps__status(&app, 0), "No Wi-Fi");
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

SUITE(maps_tests,
      TEST(arrows_pan_a_quarter_screen),
      TEST(releases_are_ignored),
      TEST(plus_minus_and_zoom_codes_zoom),
      TEST(back_exits),
      TEST(encoder_click_cycles_wheel),
      TEST(long_press_enter_and_menu_open_menu),
      TEST(touch_drag_moves_map_with_finger),
      TEST(menu_go_to_coordinates),
      TEST(go_to_accepts_space_separator_and_keeps_deep_zoom),
      TEST(go_to_rejects_bad_input),
      TEST(menu_toggles_online_and_saves),
      TEST(menu_exit_stops),
      TEST(view_saved_only_when_moved),
      TEST(status_reflects_loading_and_network),
      TEST(app_main_draws_loads_and_exits))
