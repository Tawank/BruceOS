#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "geo.h"
#include "tiles.h"

typedef struct {
    geo_point_t center;
    int zoom;
} map_view_t;

void map_view__set(map_view_t *view, double lat, double lon, int zoom);
void map_view__center(const map_view_t *view, double *out_lat, double *out_lon);
void map_view__pan(map_view_t *view, int dx, int dy);
void map_view__zoom(map_view_t *view, int delta);

size_t map_view__max_visible_tiles(int width, int height);
void map_view__draw(const map_view_t *view, tiles_t *tiles, int width, int height);

/* Visible tiles still to load, nearest to the centre first. Returns how many, at most `capacity`. */
size_t map_view__pending(
    const map_view_t *view, tiles_t *tiles, int width, int height, tile_key_t *out_keys, size_t capacity
);

/* At this zoom and on screen or one tile off it: still worth downloading. */
bool map_view__is_near(const map_view_t *view, tile_key_t key, int width, int height);
