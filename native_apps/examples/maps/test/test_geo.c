#include "geo.h"
#include "test.h"

static void world_size_doubles_per_zoom(void) {
    CHECK_EQ(geo__world_size(0), 256);
    CHECK_EQ(geo__world_size(1), 512);
    CHECK_EQ(geo__world_size(GEO_MAX_ZOOM), 256 << GEO_MAX_ZOOM);
}

static void origin_is_world_centre(void) {
    geo_point_t point = geo__from_lat_lon(0, 0, 1);
    CHECK_EQ(point.x, 256);
    CHECK_EQ(point.y, 256);
}

static void krakow_lands_on_known_tile(void) {
    geo_point_t point = geo__from_lat_lon(50.0614, 19.9372, 15);
    CHECK_EQ(point.x / GEO_TILE_SIZE, 18198);
    CHECK_EQ(point.y / GEO_TILE_SIZE, 11104);
}

static void round_trip_is_within_a_pixel(void) {
    const double places[][2] = {{50.0614, 19.9372}, {-33.8688, 151.2093}, {40.7128, -74.006}, {0.5, -0.5}};
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); ++i) {
        double lat = 0, lon = 0;
        geo__to_lat_lon(geo__from_lat_lon(places[i][0], places[i][1], 18), 18, &lat, &lon);
        CHECK_NEAR(lat, places[i][0], 0.00001);
        CHECK_NEAR(lon, places[i][1], 0.00001);
    }
}

static void poles_are_clamped_inside_world(void) {
    geo_point_t north = geo__from_lat_lon(90, 0, 3);
    geo_point_t south = geo__from_lat_lon(-90, 0, 3);
    CHECK(north.y >= 0);
    CHECK(south.y <= geo__world_size(3));
}

static void meters_per_pixel_matches_osm_table(void) {
    CHECK_NEAR(geo__meters_per_pixel(0, 0), 156543.03, 0.01);
    CHECK_NEAR(geo__meters_per_pixel(0, 15), 4.777, 0.001);
    CHECK_NEAR(geo__meters_per_pixel(60, 15), 2.389, 0.001);
}

SUITE(geo_tests,
      TEST(world_size_doubles_per_zoom),
      TEST(origin_is_world_centre),
      TEST(krakow_lands_on_known_tile),
      TEST(round_trip_is_within_a_pixel),
      TEST(poles_are_clamped_inside_world),
      TEST(meters_per_pixel_matches_osm_table))
