#include <stdlib.h>

#include "fakes.h"
#include "search.h"
#include "test.h"

static size_t parse(const char *json, search_place_t *places) {
    return search__parse(json, strlen(json), places, SEARCH_RESULTS_MAX);
}

static void parses_nominatim_results(void) {
    const char *json =
        "[{\"place_id\":1,\"licence\":\"Data \\u00a9 OSM, ODbL\",\"lat\":\"50.0547\",\"lon\":\"19.9354\","
        "\"importance\":0.61,\"name\":\"Wawel\",\"display_name\":\"Wawel, Krak\\u00f3w\","
        "\"boundingbox\":[\"50.05\",\"50.06\",\"19.93\",\"19.94\"],\"extratags\":{\"a\":\"}]\"},\"x\":null},\n"
        " {\"lat\": -33.8688, \"lon\": 151.2093, \"display_name\": \"Sydney \\\"Harbour\\\"\"}]";
    search_place_t places[SEARCH_RESULTS_MAX];
    CHECK_EQ(parse(json, places), 2u);
    CHECK_STR(places[0].name, "Wawel, Kraków");
    CHECK_NEAR(places[0].lat, 50.0547, 1e-9);
    CHECK_NEAR(places[0].lon, 19.9354, 1e-9);
    CHECK_STR(places[1].name, "Sydney \"Harbour\"");
    CHECK_NEAR(places[1].lat, -33.8688, 1e-9);
}

static void skips_places_without_coordinates(void) {
    search_place_t places[SEARCH_RESULTS_MAX];
    CHECK_EQ(parse("[{\"display_name\":\"No position\"},{\"lat\":\"1\",\"lon\":\"2\",\"display_name\":\"Ok\"}]", places),
             1u);
    CHECK_STR(places[0].name, "Ok");
}

static void rejects_broken_replies(void) {
    search_place_t places[SEARCH_RESULTS_MAX];
    const char *broken[] = {"", "[]", "{\"error\":\"x\"}", "<html>busy</html>", "[{\"lat\":\"1\",\"lon\":\"2\",\"disp"};
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) CHECK_EQ(parse(broken[i], places), 0u);
}

static void cut_body_keeps_complete_places(void) {
    const char *json = "[{\"lat\":\"1\",\"lon\":\"2\",\"display_name\":\"First\"},{\"lat\":\"3\",\"lon\"";
    search_place_t places[SEARCH_RESULTS_MAX];
    CHECK_EQ(parse(json, places), 1u);
    /* Not NUL-terminated: the parser must stop at `len`. */
    char *copy = malloc(30);
    memcpy(copy, "[{\"lat\":\"1\",\"lon\":\"2\",\"display", 30);
    CHECK_EQ(search__parse(copy, 30, places, SEARCH_RESULTS_MAX), 0u);
    free(copy);
}

static void long_names_are_cut_on_character_boundary(void) {
    char json[512];
    char name[400] = "";
    for (int i = 0; i < 60; ++i) strcat(name, "\\u00f3");
    snprintf(json, sizeof(json), "[{\"lat\":\"1\",\"lon\":\"2\",\"display_name\":\"%s\"}]", name);
    search_place_t places[SEARCH_RESULTS_MAX];
    CHECK_EQ(parse(json, places), 1u);
    CHECK_EQ(strlen(places[0].name) % 2, 0u);
    CHECK(strlen(places[0].name) < SEARCH_NAME_MAX);
}

static void keeps_at_most_max(void) {
    search_place_t places[2];
    const char *json = "[{\"lat\":\"1\",\"lon\":\"1\",\"display_name\":\"A\"},{\"lat\":\"2\",\"lon\":\"2\","
                       "\"display_name\":\"B\"},{\"lat\":\"3\",\"lon\":\"3\",\"display_name\":\"C\"}]";
    CHECK_EQ(search__parse(json, strlen(json), places, 2), 2u);
    CHECK_STR(places[1].name, "B");
}

static void parses_coordinates(void) {
    double lat = 0, lon = 0;
    CHECK(search__parse_coordinates("50.0614, 19.9372", &lat, &lon));
    CHECK_NEAR(lat, 50.0614, 1e-9);
    CHECK(search__parse_coordinates("-33.8 151.2 ", &lat, &lon));
    CHECK_NEAR(lon, 151.2, 1e-9);
    const char *bad[] = {"abc", "50.1", "91, 10", "10, 181", "10, 20 street", "Route 66"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) CHECK(!search__parse_coordinates(bad[i], &lat, &lon));
}

static void url_encodes_query_and_biases_to_view(void) {
    map_view_t view;
    map_view__set(&view, 50.06, 19.94, 12);
    char url[384];
    CHECK(search__build_url("https://n.example/search", "Kraków & co", &view, 240, 135, url, sizeof(url)));
    CHECK(strncmp(url, "https://n.example/search?q=Krak%C3%B3w%20%26%20co&format=jsonv2&limit=8&viewbox=19.", 82) == 0);
    double west = 0, north = 0, east = 0, south = 0;
    CHECK_EQ(sscanf(strstr(url, "viewbox=") + 8, "%lf,%lf,%lf,%lf", &west, &north, &east, &south), 4);
    CHECK(west < 19.94 && east > 19.94 && north > 50.06 && south < 50.06);
    CHECK(!search__build_url("https://n.example/search", "x", &view, 240, 135, url, 40));
}

static void viewbox_stays_on_the_globe(void) {
    map_view_t view;
    map_view__set(&view, 0, 179, 1);
    char url[384];
    CHECK(search__build_url("s", "x", &view, 800, 800, url, sizeof(url)));
    CHECK(strstr(url, "viewbox=-180.00000,85.") != NULL || strstr(url, ",180.00000,") != NULL);
    CHECK(strstr(url, "-181") == NULL && strstr(url, "-90.1") == NULL);
}

static void cancel_stops_the_request(void) {
    search_t search = {0};
    map_view_t view;
    map_view__set(&view, 50, 20, 10);
    fakes.http_manual = true;
    CHECK_EQ(search__start(&search, "https://n.example/search", "a", &view, 240, 135), BRUCE_OK);
    search_place_t places[SEARCH_RESULTS_MAX];
    size_t count = 9;
    CHECK_EQ(search__poll(&search, places, &count), BRUCE_ERR_BUSY);
    search__cancel(&search);
    CHECK_EQ(fakes.http_cancels, 1);
    CHECK_EQ(fakes__http_active(), 0);
    CHECK_EQ(search__poll(&search, places, &count), BRUCE_ERR_INVALID_STATE);
    CHECK_EQ(count, 0u);
}

SUITE(search_tests,
      TEST(parses_nominatim_results),
      TEST(skips_places_without_coordinates),
      TEST(rejects_broken_replies),
      TEST(cut_body_keeps_complete_places),
      TEST(long_names_are_cut_on_character_boundary),
      TEST(keeps_at_most_max),
      TEST(parses_coordinates),
      TEST(url_encodes_query_and_biases_to_view),
      TEST(viewbox_stays_on_the_globe),
      TEST(cancel_stops_the_request))
