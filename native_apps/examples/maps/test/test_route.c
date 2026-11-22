#include <math.h>
#include <string.h>

#include "route.h"
#include "route_fixture.h"
#include "test.h"

static void parse(const char *json, route_t *route, bruce_result_t expected) {
    CHECK_EQ(route__parse(json, strlen(json), route), expected);
}

static void parses_steps_and_shares_corner_points(void) {
    route_t route;
    parse(s_route_json, &route, BRUCE_OK);
    CHECK_EQ(route.point_count, 3u);
    CHECK_EQ(route.step_count, 3u);
    CHECK_EQ(route.steps[0].turn, ROUTE_DEPART);
    CHECK_EQ(route.steps[0].bearing, 90);
    CHECK_EQ(route.steps[1].turn, ROUTE_TURN);
    CHECK_EQ(route.steps[1].angle, -90);
    CHECK_STR(route.steps[1].name, "N1");
    CHECK_EQ(route.steps[0].point, 0u);
    CHECK_EQ(route.steps[1].point, 1u);
    CHECK_EQ(route.steps[2].point, 2u);
    CHECK_NEAR(route.along[1], 357, 3);
    CHECK_NEAR(route__length_m(&route), 690, 6);
    CHECK_NEAR(route__duration_s(&route), 70, 0.01);
    route__free(&route);
    CHECK(route.points == NULL && route.step_count == 0);
}

static void decodes_googles_polyline_example(void) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"code\":\"Ok\",\"routes\":[{\"legs\":[{\"steps\":[{\"geometry\":\"%s\",\"maneuver\":"
             "{\"type\":\"depart\"}},{\"geometry\":\"\",\"maneuver\":{\"type\":\"arrive\"}}]}]}]}",
             "_p~iF~ps|U_ulLnnqC_mqNvxq`@");
    route_t route;
    parse(json, &route, BRUCE_OK);
    CHECK_EQ(route.point_count, 3u);
    double lat = 0, lon = 0;
    geo__to_lat_lon(route.points[2], ROUTE_ZOOM, &lat, &lon);
    CHECK_NEAR(lat, 43.252, 0.0001);
    CHECK_NEAR(lon, -126.453, 0.0001);
    CHECK_EQ(route.steps[1].point, 2u);
    route__free(&route);
}

static void no_route_and_broken_replies(void) {
    route_t route;
    parse("{\"code\":\"NoRoute\",\"message\":\"Impossible route\"}", &route, BRUCE_ERR_NOT_FOUND);
    parse("{\"code\":\"Ok\",\"routes\":[{\"legs\":[{\"steps\":[{\"geometry\":\"_jppH", &route,
          BRUCE_ERR_INVALID_ARGUMENT);
    parse("{\"code\":\"Ok\",\"routes\":[]}", &route, BRUCE_ERR_INVALID_ARGUMENT);
    CHECK(route.points == NULL);
}

static void url_has_profile_and_lon_lat_order(void) {
    char url[256];
    CHECK(route__build_url("https://r.example", ROUTE_BIKE, 50.06, 19.93, -33.8688, 151.2093, url, sizeof(url)));
    CHECK_STR(url, "https://r.example/routed-bike/route/v1/driving/19.93000,50.06000;151.20930,-33.86880"
                   "?overview=false&steps=true");
    CHECK(!route__build_url("https://r.example", ROUTE_CAR, 0, 0, 0, 0, url, 40));
}

static void instruction(route_turn_t turn, int angle, int exit, const char *name, const char *expected) {
    route_step_t step = {.turn = turn, .angle = (int16_t)angle, .exit = (uint8_t)exit, .bearing = 200};
    snprintf(step.name, sizeof(step.name), "%s", name);
    char text[ROUTE_TEXT_MAX];
    route__instruction(&step, text, sizeof(text));
    CHECK_STR(text, expected);
}

static void instructions_read_naturally(void) {
    instruction(ROUTE_DEPART, 0, 0, "Main", "Head south on Main");
    instruction(ROUTE_TURN, -90, 0, "Wiślna", "Turn left onto Wiślna");
    instruction(ROUTE_TURN, 45, 0, "", "Turn slightly right");
    instruction(ROUTE_TURN, 180, 0, "", "Make a U-turn");
    instruction(ROUTE_CONTINUE, 0, 0, "Grodzka", "Continue onto Grodzka");
    instruction(ROUTE_ROUNDABOUT, 90, 2, "A4", "At the roundabout take exit 2 onto A4");
    instruction(ROUTE_FORK, -45, 0, "", "Keep left");
    instruction(ROUTE_OFF_RAMP, 45, 0, "", "Take the exit on the right");
    instruction(ROUTE_END_OF_ROAD, 90, 0, "", "At the end of the road turn right");
    instruction(ROUTE_ARRIVE, 0, 0, "North", "Arrive at the destination");
}

static void formats_distances_and_times(void) {
    char text[24];
    route__format_countdown(text, sizeof(text), 347);
    CHECK_STR(text, "350 m");
    route__format_countdown(text, sizeof(text), 44);
    CHECK_STR(text, "40 m");
    route__format_countdown(text, sizeof(text), 1260);
    CHECK_STR(text, "1.3 km");
    route__format_duration(text, sizeof(text), 10);
    CHECK_STR(text, "1 min");
    route__format_duration(text, sizeof(text), 3900);
    CHECK_STR(text, "1 h 05 min");
}

static void tracking_finds_next_turn_and_whats_left(void) {
    route_t route;
    parse(s_route_json, &route, BRUCE_OK);
    route_progress_t progress;
    /* Halfway along Main, a few metres north of it. */
    route__track(&route, 50.06003, 19.9325, 0, &progress);
    CHECK_EQ(progress.next, 1u);
    CHECK_NEAR(progress.to_next_m, 178, 4);
    CHECK_NEAR(progress.off_m, 3.3, 1);
    CHECK_NEAR(progress.left_m, 512, 6);
    CHECK_NEAR(progress.left_s, 55, 1);
    CHECK_EQ(progress.segment, 0u);
    /* On North: the next step is arriving. */
    route__track(&route, 50.0615, 19.935, progress.segment, &progress);
    CHECK_EQ(progress.next, 2u);
    CHECK_NEAR(progress.to_next_m, 167, 4);
    CHECK_EQ(progress.segment, 1u);
    /* Far away. */
    route__track(&route, 50.07, 19.95, progress.segment, &progress);
    CHECK(progress.off_m > 500);
    route__free(&route);
}

SUITE(route_tests,
      TEST(parses_steps_and_shares_corner_points),
      TEST(decodes_googles_polyline_example),
      TEST(no_route_and_broken_replies),
      TEST(url_has_profile_and_lon_lat_order),
      TEST(instructions_read_naturally),
      TEST(formats_distances_and_times),
      TEST(tracking_finds_next_turn_and_whats_left))
