#include <stdlib.h>

#include "fakes.h"
#include "map_view.h"
#include "test.h"

#define LAND 0xF77D
#define WATER 0xA69B

static tiles_t tiles;

static void init_tiles(void) { tiles__init(&tiles, 8, "/sdcard/maps/tiles", "", false); }

static void put_tile(size_t slot, tile_key_t key, image_bitmap_t bitmap) {
    tiles.slots[slot] = (tile_slot_t){.key = key, .bitmap = bitmap, .last_used = 1};
}

/* View centred in the middle of tile (x, y); on 240x135 only that tile is visible. */
static map_view_t view_at_tile(int zoom, int x, int y) {
    return (map_view_t){.zoom = zoom, .center = {x * GEO_TILE_SIZE + 128, y * GEO_TILE_SIZE + 128}};
}

static void set_and_center_round_trip(void) {
    map_view_t view;
    map_view__set(&view, 50.0614, 19.9372, 16);
    double lat = 0, lon = 0;
    map_view__center(&view, &lat, &lon);
    CHECK_EQ(view.zoom, 16);
    CHECK_NEAR(lat, 50.0614, 0.0001);
    CHECK_NEAR(lon, 19.9372, 0.0001);
}

static void pan_wraps_x_and_clamps_y(void) {
    map_view_t view = {.zoom = 1, .center = {10, 10}};
    map_view__pan(&view, -20, -20);
    CHECK_EQ(view.center.x, 512 - 10);
    CHECK_EQ(view.center.y, 0);
    map_view__pan(&view, 30, 9999);
    CHECK_EQ(view.center.x, 20);
    CHECK_EQ(view.center.y, 511);
}

static void zoom_scales_centre_and_stops_at_limits(void) {
    map_view_t view = {.zoom = 5, .center = {1000, 600}};
    map_view__zoom(&view, 1);
    CHECK_EQ(view.zoom, 6);
    CHECK_EQ(view.center.x, 2000);
    CHECK_EQ(view.center.y, 1200);
    map_view__zoom(&view, -2);
    CHECK_EQ(view.zoom, 4);
    CHECK_EQ(view.center.x, 500);

    map_view_t top = {.zoom = GEO_MAX_ZOOM, .center = {7, 7}};
    map_view__zoom(&top, 1);
    CHECK_EQ(top.zoom, GEO_MAX_ZOOM);
    CHECK_EQ(top.center.x, 7);
    map_view_t bottom = {.zoom = GEO_MIN_ZOOM, .center = {7, 7}};
    map_view__zoom(&bottom, -1);
    CHECK_EQ(bottom.zoom, GEO_MIN_ZOOM);
}

static void max_visible_tiles_covers_any_offset(void) {
    CHECK_EQ(map_view__max_visible_tiles(240, 135), 4u);
    CHECK_EQ(map_view__max_visible_tiles(240, 320), 6u);
    CHECK_EQ(map_view__max_visible_tiles(320, 240), 6u);
    CHECK_EQ(map_view__max_visible_tiles(256, 256), 4u);
}

static void pending_starts_at_centre_tile(void) {
    init_tiles();
    map_view_t view = view_at_tile(5, 10, 12);
    tile_key_t keys[8];
    CHECK_EQ(map_view__pending(&view, &tiles, 240, 135, keys, 8), 1u);
    CHECK_EQ(keys[0].z, 5);
    CHECK_EQ(keys[0].x, 10);
    CHECK_EQ(keys[0].y, 12);
    tiles__deinit(&tiles);
}

static void pending_is_sorted_nearest_first(void) {
    init_tiles();
    map_view_t view = {.zoom = 6, .center = {10 * GEO_TILE_SIZE + 200, 12 * GEO_TILE_SIZE + 128}};
    tile_key_t keys[8];
    size_t count = map_view__pending(&view, &tiles, 240, 320, keys, 8);
    CHECK_EQ(count, 6u);
    CHECK_EQ(keys[0].x, 10);
    CHECK_EQ(keys[0].y, 12);
    int32_t previous = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t dx = keys[i].x * GEO_TILE_SIZE + 128 - view.center.x;
        int32_t dy = keys[i].y * GEO_TILE_SIZE + 128 - view.center.y;
        int32_t distance = abs(dx) + abs(dy);
        CHECK(distance >= previous);
        previous = distance;
    }
    CHECK_EQ(map_view__pending(&view, &tiles, 240, 320, keys, 2), 2u);
    CHECK_EQ(keys[0].x, 10);
    CHECK_EQ(keys[0].y, 12);
    tiles__deinit(&tiles);
}

static void pending_skips_cached_missing_and_downloading(void) {
    init_tiles();
    map_view_t view = {.zoom = 5, .center = {10 * GEO_TILE_SIZE, 12 * GEO_TILE_SIZE}};
    tile_key_t keys[8];
    CHECK_EQ(map_view__pending(&view, &tiles, 240, 135, keys, 8), 4u);

    put_tile(0, (tile_key_t){5, 10, 12}, fakes__bitmap(GEO_TILE_SIZE, 1, false));
    tiles.missing[0] = (tile_key_t){5, 9, 12};
    tiles.missing_count = 1;
    tiles.downloads[0] = (tile_download_t){.key = {5, 10, 11}, .job = 99};
    tiles.download_count = 1;
    CHECK_EQ(map_view__pending(&view, &tiles, 240, 135, keys, 8), 1u);
    CHECK_EQ(keys[0].x, 9);
    CHECK_EQ(keys[0].y, 11);
    tiles.download_count = 0;
    tiles__deinit(&tiles);
}

static void pending_wraps_tile_x(void) {
    init_tiles();
    map_view_t view = {.zoom = 2, .center = {0, 2 * GEO_TILE_SIZE}};
    tile_key_t keys[8];
    CHECK_EQ(map_view__pending(&view, &tiles, 240, 135, keys, 8), 4u);
    for (size_t i = 0; i < 4; ++i) CHECK(keys[i].x == 0 || keys[i].x == 3);
    tiles__deinit(&tiles);
}

static void near_covers_screen_plus_one_tile(void) {
    map_view_t view = view_at_tile(5, 10, 12);
    CHECK(map_view__is_near(&view, (tile_key_t){5, 10, 12}, 240, 135));
    CHECK(map_view__is_near(&view, (tile_key_t){5, 11, 13}, 240, 135));
    CHECK(map_view__is_near(&view, (tile_key_t){5, 9, 11}, 240, 135));
    CHECK(!map_view__is_near(&view, (tile_key_t){5, 12, 12}, 240, 135));
    CHECK(!map_view__is_near(&view, (tile_key_t){5, 10, 14}, 240, 135));
    CHECK(!map_view__is_near(&view, (tile_key_t){6, 20, 24}, 240, 135));

    map_view_t edge = {.zoom = 2, .center = {10, 2 * GEO_TILE_SIZE}};
    CHECK(map_view__is_near(&edge, (tile_key_t){2, 3, 2}, 240, 135));
}

static void draw_places_cached_tile(void) {
    init_tiles();
    image_bitmap_t bitmap = fakes__bitmap(GEO_TILE_SIZE, 42, false);
    put_tile(0, (tile_key_t){5, 10, 12}, bitmap);
    map_view_t view = view_at_tile(5, 10, 12);
    map_view__draw(&view, &tiles, 240, 135);

    bool found = false;
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        const fake_draw_t *draw = &fakes.draws[i];
        if (draw->kind == FAKE_RGB_BITMAP && draw->pixels == bitmap.pixels) {
            found = true;
            CHECK_EQ(draw->x, 120 - 128);
            CHECK_EQ(draw->y, 67 - 128);
            CHECK_EQ(draw->w, GEO_TILE_SIZE);
            CHECK_EQ(draw->h, GEO_TILE_SIZE);
        }
    }
    CHECK(found);
    tiles__deinit(&tiles);
}

static void draw_uses_placeholder_without_data(void) {
    init_tiles();
    map_view_t view = view_at_tile(5, 10, 12);
    map_view__draw(&view, &tiles, 240, 135);
    size_t land = 0;
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        const fake_draw_t *draw = &fakes.draws[i];
        land += draw->color == LAND && draw->w == GEO_TILE_SIZE && draw->h == GEO_TILE_SIZE;
    }
    CHECK_EQ(land, 1u);
    CHECK_EQ(fakes__count(FAKE_RGB_BITMAP), 0u);
    tiles__deinit(&tiles);
}

static void draw_fills_water_outside_world(void) {
    init_tiles();
    map_view_t view = {.zoom = 1, .center = {256, 0}};
    map_view__draw(&view, &tiles, 240, 135);
    size_t water = 0;
    for (size_t i = 0; i < fakes.draw_count; ++i) water += fakes.draws[i].color == WATER;
    CHECK_EQ(water, 2u);
    tiles__deinit(&tiles);
}

static void draw_scales_up_parent_quarter(void) {
    init_tiles();
    put_tile(0, (tile_key_t){4, 5, 6}, fakes__bitmap(GEO_TILE_SIZE, 0, true));
    map_view_t view = view_at_tile(5, 11, 12);
    map_view__draw(&view, &tiles, 240, 135);

    const fake_draw_t *first = NULL;
    size_t strips = 0;
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        const fake_draw_t *draw = &fakes.draws[i];
        if (draw->kind != FAKE_RGB_BITMAP || draw->h != 2) continue;
        if (first == NULL && draw->x == 120 - 128) first = draw;
        strips += draw->x == 120 - 128;
    }
    CHECK(first != NULL);
    if (first != NULL) {
        int source_row = (first->y - (67 - 128)) / 2;
        uint16_t expected = (uint16_t)(source_row * GEO_TILE_SIZE + 128);
        CHECK_EQ(first->first_pixels[0], expected);
        CHECK_EQ(first->first_pixels[1], expected);
        CHECK_EQ(first->first_pixels[2], (uint16_t)(expected + 1));
    }
    CHECK(strips >= 135 / 2 && strips <= 135 / 2 + 2);
    tiles__deinit(&tiles);
}

/* Zoom 5 centred in z5 tile (10, 12), which lies in z4 tile (5, 6). */
static map_view_t big_view(void) {
    map_view_t view = view_at_tile(5, 10, 12);
    view.big = true;
    return view;
}

static void big_loads_tiles_one_zoom_out(void) {
    init_tiles();
    map_view_t view = big_view();
    tile_key_t keys[8];
    CHECK_EQ(map_view__pending(&view, &tiles, 240, 135, keys, 8), 1u);
    CHECK_EQ(keys[0].z, 4);
    CHECK_EQ(keys[0].x, 5);
    CHECK_EQ(keys[0].y, 6);
    CHECK(map_view__is_near(&view, (tile_key_t){4, 5, 6}, 240, 135));
    CHECK(!map_view__is_near(&view, (tile_key_t){5, 10, 12}, 240, 135));
    tiles__deinit(&tiles);
}

static void big_stays_at_min_zoom(void) {
    init_tiles();
    map_view_t view = {.zoom = GEO_MIN_ZOOM, .big = true, .center = {256, 256}};
    tile_key_t keys[8];
    CHECK(map_view__pending(&view, &tiles, 240, 135, keys, 8) > 0);
    CHECK_EQ(keys[0].z, GEO_MIN_ZOOM);
    tiles__deinit(&tiles);
}

static void big_draws_tile_doubled(void) {
    init_tiles();
    put_tile(0, (tile_key_t){4, 5, 6}, fakes__bitmap(GEO_TILE_SIZE, 0, true));
    map_view_t view = big_view();
    map_view__draw(&view, &tiles, 240, 135);

    /* Tile (5, 6) starts at z5 pixel (2560, 3072); the screen's top-left is (2568, 3133). */
    const fake_draw_t *first = NULL;
    size_t strips = 0;
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        const fake_draw_t *draw = &fakes.draws[i];
        if (draw->kind != FAKE_RGB_BITMAP || draw->h != 2 || draw->w != 2 * GEO_TILE_SIZE) continue;
        if (first == NULL) first = draw;
        strips += draw->x == -8;
    }
    CHECK(first != NULL);
    if (first != NULL) {
        int source_row = (first->y + 61) / 2;
        uint16_t expected = (uint16_t)(source_row * GEO_TILE_SIZE);
        CHECK_EQ(first->first_pixels[0], expected);
        CHECK_EQ(first->first_pixels[1], expected);
        CHECK_EQ(first->first_pixels[2], (uint16_t)(expected + 1));
    }
    CHECK(strips >= 135 / 2 && strips <= 135 / 2 + 2);
    tiles__deinit(&tiles);
}

SUITE(map_view_tests,
      TEST(set_and_center_round_trip),
      TEST(pan_wraps_x_and_clamps_y),
      TEST(zoom_scales_centre_and_stops_at_limits),
      TEST(max_visible_tiles_covers_any_offset),
      TEST(pending_starts_at_centre_tile),
      TEST(pending_is_sorted_nearest_first),
      TEST(pending_skips_cached_missing_and_downloading),
      TEST(pending_wraps_tile_x),
      TEST(near_covers_screen_plus_one_tile),
      TEST(draw_places_cached_tile),
      TEST(draw_uses_placeholder_without_data),
      TEST(draw_fills_water_outside_world),
      TEST(draw_scales_up_parent_quarter),
      TEST(big_loads_tiles_one_zoom_out),
      TEST(big_stays_at_min_zoom),
      TEST(big_draws_tile_doubled))
