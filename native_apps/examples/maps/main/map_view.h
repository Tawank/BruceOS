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

/* Visible tile still to load, nearest to the centre first. Returns how many are pending. */
size_t map_view__next_pending(
    const map_view_t *view, tiles_t *tiles, int width, int height, tile_key_t *out_key
);
