#include "map_view.h"

#include <stdint.h>
#include <stdlib.h>

#include "core_sdk/display.h"

#define MAP_VIEW_LAND 0xF77D
#define MAP_VIEW_GRID 0xE6FA
#define MAP_VIEW_WATER 0xA69B
#define MAP_VIEW_GRID_STEP 64

typedef struct {
    int32_t left;
    int32_t top;
    int32_t first_x;
    int32_t first_y;
    int32_t last_x;
    int32_t last_y;
} map_view_range_t;

static int32_t map_view__floor_div(int32_t value, int32_t divisor) {
    int32_t quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

static map_view_range_t map_view__range(const map_view_t *view, int width, int height) {
    map_view_range_t range = {.left = view->center.x - width / 2, .top = view->center.y - height / 2};
    range.first_x = map_view__floor_div(range.left, GEO_TILE_SIZE);
    range.first_y = map_view__floor_div(range.top, GEO_TILE_SIZE);
    range.last_x = map_view__floor_div(range.left + width - 1, GEO_TILE_SIZE);
    range.last_y = map_view__floor_div(range.top + height - 1, GEO_TILE_SIZE);
    return range;
}

static bool map_view__key(const map_view_t *view, int32_t tile_x, int32_t tile_y, tile_key_t *out_key) {
    int32_t count = 1 << view->zoom;
    if (tile_y < 0 || tile_y >= count) return false;
    int x = (int)(((tile_x % count) + count) % count);
    *out_key = (tile_key_t){.z = view->zoom, .x = x, .y = (int)tile_y};
    return true;
}

void map_view__set(map_view_t *view, double lat, double lon, int zoom) {
    view->zoom = zoom;
    view->center = geo__from_lat_lon(lat, lon, zoom);
}

void map_view__center(const map_view_t *view, double *out_lat, double *out_lon) {
    geo__to_lat_lon(view->center, view->zoom, out_lat, out_lon);
}

void map_view__pan(map_view_t *view, int dx, int dy) {
    int32_t size = geo__world_size(view->zoom);
    view->center.x = ((view->center.x + dx) % size + size) % size;
    view->center.y += dy;
    if (view->center.y < 0) view->center.y = 0;
    if (view->center.y >= size) view->center.y = size - 1;
}

void map_view__zoom(map_view_t *view, int delta) {
    int zoom = view->zoom + delta;
    if (zoom < GEO_MIN_ZOOM || zoom > GEO_MAX_ZOOM) return;
    if (delta > 0) {
        view->center.x <<= delta;
        view->center.y <<= delta;
    } else {
        view->center.x >>= -delta;
        view->center.y >>= -delta;
    }
    view->zoom = zoom;
}

size_t map_view__max_visible_tiles(int width, int height) {
    size_t columns = (size_t)(width + GEO_TILE_SIZE - 2) / GEO_TILE_SIZE + 1;
    size_t rows = (size_t)(height + GEO_TILE_SIZE - 2) / GEO_TILE_SIZE + 1;
    return columns * rows;
}

static void map_view__draw_placeholder(int x, int y) {
    display__fill_rect(x, y, GEO_TILE_SIZE, GEO_TILE_SIZE, MAP_VIEW_LAND);
    for (int offset = 0; offset < GEO_TILE_SIZE; offset += MAP_VIEW_GRID_STEP) {
        display__fill_rect(x + offset, y, 1, GEO_TILE_SIZE, MAP_VIEW_GRID);
        display__fill_rect(x, y + offset, GEO_TILE_SIZE, 1, MAP_VIEW_GRID);
    }
}

/* Shows a quarter of the parent tile at twice the size while the real one loads. */
static bool map_view__draw_from_parent(tiles_t *tiles, tile_key_t key, int x, int y, int height) {
    if (key.z <= GEO_MIN_ZOOM) return false;
    const image_bitmap_t *parent = tiles__get(tiles, (tile_key_t){key.z - 1, key.x / 2, key.y / 2});
    if (parent == NULL) return false;

    const int half = GEO_TILE_SIZE / 2;
    const uint16_t *quarter = parent->pixels + (key.y & 1) * half * GEO_TILE_SIZE + (key.x & 1) * half;
    uint16_t rows[2 * GEO_TILE_SIZE];
    for (int row = 0; row < GEO_TILE_SIZE; row += 2) {
        if (y + row + 2 <= 0 || y + row >= height) continue;
        const uint16_t *source = quarter + (row / 2) * GEO_TILE_SIZE;
        for (int column = 0; column < GEO_TILE_SIZE; ++column) {
            rows[column] = rows[GEO_TILE_SIZE + column] = source[column / 2];
        }
        display__draw_rgb_bitmap(x, y + row, rows, GEO_TILE_SIZE, 2);
    }
    return true;
}

void map_view__draw(const map_view_t *view, tiles_t *tiles, int width, int height) {
    map_view_range_t range = map_view__range(view, width, height);
    for (int32_t tile_y = range.first_y; tile_y <= range.last_y; ++tile_y) {
        for (int32_t tile_x = range.first_x; tile_x <= range.last_x; ++tile_x) {
            int x = tile_x * GEO_TILE_SIZE - range.left;
            int y = tile_y * GEO_TILE_SIZE - range.top;
            tile_key_t key;
            if (!map_view__key(view, tile_x, tile_y, &key)) {
                display__fill_rect(x, y, GEO_TILE_SIZE, GEO_TILE_SIZE, MAP_VIEW_WATER);
                continue;
            }
            const image_bitmap_t *bitmap = tiles__get(tiles, key);
            if (bitmap != NULL) {
                display__draw_rgb_bitmap(x, y, bitmap->pixels, GEO_TILE_SIZE, GEO_TILE_SIZE);
            } else if (!map_view__draw_from_parent(tiles, key, x, y, height)) {
                map_view__draw_placeholder(x, y);
            }
        }
    }
}

size_t map_view__next_pending(
    const map_view_t *view, tiles_t *tiles, int width, int height, tile_key_t *out_key
) {
    map_view_range_t range = map_view__range(view, width, height);
    size_t pending = 0;
    int32_t best_distance = INT32_MAX;
    for (int32_t tile_y = range.first_y; tile_y <= range.last_y; ++tile_y) {
        for (int32_t tile_x = range.first_x; tile_x <= range.last_x; ++tile_x) {
            tile_key_t key;
            if (!map_view__key(view, tile_x, tile_y, &key)) continue;
            if (tiles__get(tiles, key) != NULL || tiles__is_missing(tiles, key)) continue;
            int32_t dx = tile_x * GEO_TILE_SIZE + GEO_TILE_SIZE / 2 - view->center.x;
            int32_t dy = tile_y * GEO_TILE_SIZE + GEO_TILE_SIZE / 2 - view->center.y;
            int32_t distance = abs(dx) + abs(dy);
            if (distance < best_distance) {
                best_distance = distance;
                *out_key = key;
            }
            pending++;
        }
    }
    return pending;
}
