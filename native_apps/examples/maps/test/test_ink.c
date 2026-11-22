#include "ink.h"
#include "test.h"

#define W 8
#define H 8
#define BLACK 0x0000
#define WHITE 0xFFFF

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void fill(uint16_t *pixels, uint16_t color) {
    for (int i = 0; i < W * H; ++i) pixels[i] = color;
}

static void only_black_and_white_remain(void) {
    uint16_t pixels[W * H];
    for (int i = 0; i < W * H; ++i) pixels[i] = (uint16_t)(i * 1031);
    ink__convert(pixels, W, H);
    for (int i = 0; i < W * H; ++i) CHECK(pixels[i] == BLACK || pixels[i] == WHITE);
}

static void plain_land_and_pale_areas_turn_white(void) {
    uint16_t pixels[W * H];
    fill(pixels, rgb(0xf2, 0xef, 0xe9));
    for (int y = 4; y < H; ++y)
        for (int x = 0; x < W; ++x) pixels[y * W + x] = rgb(0xd9, 0xd0, 0xc9); /* buildings */
    ink__convert(pixels, W, H);
    for (int i = 0; i < W * H; ++i) CHECK_EQ(pixels[i], WHITE);
}

static void road_gets_outlined_where_it_meets_land(void) {
    uint16_t pixels[W * H];
    fill(pixels, rgb(0xf2, 0xef, 0xe9));
    for (int y = 0; y < H; ++y)
        for (int x = 3; x < 6; ++x) pixels[y * W + x] = WHITE;
    ink__convert(pixels, W, H);
    CHECK_EQ(pixels[4 * W + 3], BLACK);
    CHECK_EQ(pixels[4 * W + 4], WHITE);
    CHECK_EQ(pixels[4 * W + 5], BLACK);
    CHECK_EQ(pixels[4 * W + 1], WHITE);
}

static void label_halo_is_not_a_road(void) {
    uint16_t pixels[W * H];
    fill(pixels, rgb(0xf2, 0xef, 0xe9));
    for (int y = 2; y < 6; ++y)
        for (int x = 2; x < 6; ++x) pixels[y * W + x] = WHITE;
    pixels[3 * W + 3] = rgb(0x33, 0x33, 0x33);
    ink__convert(pixels, W, H);
    CHECK_EQ(pixels[3 * W + 3], BLACK);
    CHECK_EQ(pixels[2 * W + 2], WHITE);
    CHECK_EQ(pixels[5 * W + 5], WHITE);
}

static void text_stays_black(void) {
    uint16_t pixels[W * H];
    fill(pixels, WHITE);
    pixels[3 * W + 3] = rgb(0x33, 0x33, 0x33);
    ink__convert(pixels, W, H);
    CHECK_EQ(pixels[3 * W + 3], BLACK);
}

static void water_is_sparse_dots(void) {
    uint16_t pixels[W * H];
    fill(pixels, rgb(0xaa, 0xd3, 0xdf));
    ink__convert(pixels, W, H);
    int black = 0;
    for (int i = 0; i < W * H; ++i) black += pixels[i] == BLACK;
    CHECK_EQ(black, 4);
    CHECK_EQ(pixels[0], BLACK);
    CHECK_EQ(pixels[1], WHITE);
}

SUITE(ink_tests,
      TEST(only_black_and_white_remain),
      TEST(plain_land_and_pale_areas_turn_white),
      TEST(road_gets_outlined_where_it_meets_land),
      TEST(label_halo_is_not_a_road),
      TEST(text_stays_black),
      TEST(water_is_sparse_dots))
