#include "ink.h"

#include <stdbool.h>

/* What each pixel shows, kept in the low bits while converting. */
enum { INK_OUTSIDE, INK_DARK, INK_ROAD, INK_WATER, INK_OTHER };
#define INK_CLASS_MASK 0x7
#define INK_BLACK_BIT 0x8000
/* Text, railways and casings; coloured labels are darker than any area fill. */
#define INK_DARK_LUMA 130
#define INK_GRAY_LUMA 180
#define INK_GRAY_SPREAD 24
/* White next to text within this many pixels is a label's halo, not a road. */
#define INK_HALO 2

static int ink__red(uint16_t c) { return (c >> 11) << 3; }
static int ink__green(uint16_t c) { return ((c >> 5) & 0x3F) << 2; }
static int ink__blue(uint16_t c) { return (c & 0x1F) << 3; }

static int ink__max(int a, int b) { return a > b ? a : b; }
static int ink__min(int a, int b) { return a < b ? a : b; }

static uint16_t ink__classify(uint16_t c) {
    int r = ink__red(c), g = ink__green(c), b = ink__blue(c);
    int luma = (r * 77 + g * 150 + b * 29) >> 8;
    int high = ink__max(r, ink__max(g, b));
    int low = ink__min(r, ink__min(g, b));
    if (luma < INK_DARK_LUMA || (high - low < INK_GRAY_SPREAD && luma < INK_GRAY_LUMA)) return INK_DARK;
    /* White minor roads, and the yellow, orange and pink of major ones. */
    if (low >= 0xF0 || (r >= 0xE0 && r - b > 50 && luma > 170)) return INK_ROAD;
    if (b > r + 40 && g > r + 20) return INK_WATER;
    return INK_OTHER;
}

static int ink__class_at(const uint16_t *pixels, int width, int height, int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height) return INK_OUTSIDE;
    return pixels[y * width + x] & INK_CLASS_MASK;
}

static bool ink__road_edge(const uint16_t *pixels, int width, int height, int x, int y) {
    static const int8_t sides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    bool edge = false;
    for (int i = 0; i < 4; ++i) {
        int side = ink__class_at(pixels, width, height, x + sides[i][0], y + sides[i][1]);
        if (side != INK_OUTSIDE && side != INK_ROAD) edge = true;
    }
    if (!edge) return false;
    for (int dy = -INK_HALO; dy <= INK_HALO; ++dy) {
        for (int dx = -INK_HALO; dx <= INK_HALO; ++dx) {
            if (ink__class_at(pixels, width, height, x + dx, y + dy) == INK_DARK) return false;
        }
    }
    return true;
}

void ink__convert(uint16_t *pixels, int width, int height) {
    int count = width * height;
    for (int i = 0; i < count; ++i) pixels[i] = ink__classify(pixels[i]);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint16_t *pixel = &pixels[y * width + x];
            int kind = *pixel & INK_CLASS_MASK;
            bool black = kind == INK_DARK;
            if (kind == INK_WATER) black = x % 4 == 0 && y % 4 == 0;
            if (kind == INK_ROAD) black = ink__road_edge(pixels, width, height, x, y);
            if (black) *pixel |= INK_BLACK_BIT;
        }
    }
    for (int i = 0; i < count; ++i) pixels[i] = (pixels[i] & INK_BLACK_BIT) ? 0x0000 : 0xFFFF;
}
