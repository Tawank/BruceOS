#pragma once

#include <stdint.h>

#define GEO_TILE_SIZE 256
#define GEO_MIN_ZOOM 1
#define GEO_MAX_ZOOM 19

/* Web Mercator position in world pixels at one zoom level. */
typedef struct {
    int32_t x;
    int32_t y;
} geo_point_t;

int32_t geo__world_size(int zoom);
geo_point_t geo__from_lat_lon(double lat, double lon, int zoom);
void geo__to_lat_lon(geo_point_t point, int zoom, double *out_lat, double *out_lon);
double geo__meters_per_pixel(double lat, int zoom);
