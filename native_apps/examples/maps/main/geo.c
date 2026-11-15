#include "geo.h"

#include <math.h>

#define GEO_PI 3.14159265358979323846
#define GEO_MAX_LAT 85.0511
#define GEO_EARTH_CIRCUMFERENCE_M 40075016.686

int32_t geo__world_size(int zoom) { return (int32_t)GEO_TILE_SIZE << zoom; }

geo_point_t geo__from_lat_lon(double lat, double lon, int zoom) {
    if (lat > GEO_MAX_LAT) lat = GEO_MAX_LAT;
    if (lat < -GEO_MAX_LAT) lat = -GEO_MAX_LAT;
    double size = geo__world_size(zoom);
    double s = sin(lat * GEO_PI / 180.0);
    double x = (lon + 180.0) / 360.0 * size;
    double y = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * GEO_PI)) * size;
    return (geo_point_t){.x = (int32_t)floor(x), .y = (int32_t)floor(y)};
}

void geo__to_lat_lon(geo_point_t point, int zoom, double *out_lat, double *out_lon) {
    double size = geo__world_size(zoom);
    double n = GEO_PI * (1.0 - 2.0 * point.y / size);
    *out_lon = point.x / size * 360.0 - 180.0;
    *out_lat = (2.0 * atan2(exp(n), 1.0) - GEO_PI / 2.0) * 180.0 / GEO_PI;
}

double geo__meters_per_pixel(double lat, int zoom) {
    return GEO_EARTH_CIRCUMFERENCE_M * cos(lat * GEO_PI / 180.0) / geo__world_size(zoom);
}
