#include "view3d.h"

#include <math.h>
#include <stdint.h>

#include "core_sdk/display.h"
#include "route.h"

#define VIEW3D_SKY 0x9E3E
#define VIEW3D_HAZE 0xD71D
#define VIEW3D_LAND 0xF77D
#define VIEW3D_WATER 0xA69B
#define VIEW3D_ROUTE 0x2B9F
#define VIEW3D_ME 0x1C9F
#define VIEW3D_PIN 0xE8E4
#define VIEW3D_ROW_MAX 800
#define VIEW3D_HAZE_ROWS 6
#define VIEW3D_SAMPLE_X 16
#define VIEW3D_SAMPLE_Y 10

/* Rows below the horizon are a ground plane: row y shows the ground
 * `focal * s` ahead of the camera at `s` tile pixels per screen pixel, with
 * s = k / (y - horizon). The view centre sits on the marker row at the 2D
 * view's scale; s is capped at `s_max`, above which is sky. */
typedef struct {
    int width, height;
    int zoom;      /* Tile zoom. */
    int32_t cx, cy; /* View centre at the tile zoom. */
    float fx, fy;  /* Ahead, in map axes (x east, y south). */
    float rx, ry;  /* Right. */
    float horizon, marker_y, focal, k, s_mark, s_max;
} view3d_camera_t;

static uint16_t s_row[VIEW3D_ROW_MAX];

static view3d_camera_t view3d__camera(const view3d_t *view3d, const map_view_t *view, int width, int height) {
    view3d_camera_t cam = {.width = width, .height = height};
    cam.zoom = view->zoom > GEO_MIN_ZOOM ? view->zoom - 1 : view->zoom;
    int shift = view->zoom - cam.zoom;
    cam.cx = view->center.x >> shift;
    cam.cy = view->center.y >> shift;
    float angle = view3d->heading_deg * (float)M_PI / 180.0f;
    cam.fx = sinf(angle);
    cam.fy = -cosf(angle);
    cam.rx = cosf(angle);
    cam.ry = sinf(angle);
    cam.horizon = height * 0.22f;
    cam.marker_y = height * 0.72f;
    cam.focal = (float)height;
    cam.s_mark = 1.0f / (float)(1 << shift);
    cam.k = cam.s_mark * (cam.marker_y - cam.horizon);
    cam.s_max = cam.s_mark * 5.0f;
    return cam;
}

/* First row showing ground. */
static int view3d__ground_top(const view3d_camera_t *cam) { return (int)ceilf(cam->horizon + cam->k / cam->s_max); }

/* Ground under a screen point, in tile pixels from the centre. */
static void view3d__ground(const view3d_camera_t *cam, float sx, float y, float *gx, float *gy) {
    float s = cam->k / (y - cam->horizon);
    float ahead = cam->focal * (s - cam->s_mark);
    float right = (sx - cam->width / 2.0f) * s;
    *gx = cam->fx * ahead + cam->rx * right;
    *gy = cam->fy * ahead + cam->ry * right;
}

static bool view3d__key(const view3d_camera_t *cam, int32_t tile_x, int32_t tile_y, tile_key_t *out) {
    int32_t count = 1 << cam->zoom;
    if (tile_y < 0 || tile_y >= count) return false;
    *out = (tile_key_t){.z = cam->zoom, .x = (int)(((tile_x % count) + count) % count), .y = (int)tile_y};
    return true;
}

bool view3d__set_heading(view3d_t *view3d, float heading_deg) {
    float heading = fmodf(heading_deg, 360.0f);
    if (heading < 0) heading += 360.0f;
    if (fabsf(heading - view3d->heading_deg) < 1.0f) return false;
    view3d->heading_deg = heading;
    return true;
}

void view3d__turn(view3d_t *view3d, float degrees) { (void)view3d__set_heading(view3d, view3d->heading_deg + degrees); }

void view3d__rotate_pan(const view3d_t *view3d, int *dx, int *dy) {
    float angle = view3d->heading_deg * (float)M_PI / 180.0f;
    float c = cosf(angle), s = sinf(angle);
    float x = c * *dx - s * *dy;
    float y = s * *dx + c * *dy;
    *dx = (int)floorf(x + 0.5f);
    *dy = (int)floorf(y + 0.5f);
}

/* Samples the ground from the bottom up, so the nearest tiles come first;
 * no more than the cache holds with room to spare, or loading would evict
 * tiles still on screen. */
size_t view3d__pending(view3d_t *view3d, const map_view_t *view, tiles_t *tiles, int width, int height,
                       tile_key_t *out_keys, size_t capacity) {
    view3d_camera_t cam = view3d__camera(view3d, view, width, height);
    size_t limit = tiles->slot_count > 2 ? tiles->slot_count - 2 : 1;
    if (limit > VIEW3D_KEYS_MAX) limit = VIEW3D_KEYS_MAX;
    view3d->key_count = 0;
    int top = view3d__ground_top(&cam);
    for (int y = height - 1; y >= top && view3d->key_count < limit; y -= VIEW3D_SAMPLE_Y) {
        for (int i = 0; i <= width / VIEW3D_SAMPLE_X + 1 && view3d->key_count < limit; ++i) {
            int offset = (i + 1) / 2 * VIEW3D_SAMPLE_X;
            int sx = width / 2 + (i % 2 == 1 ? offset : -offset);
            if (sx < 0) sx = 0;
            if (sx >= width) sx = width - 1;
            float gx, gy;
            view3d__ground(&cam, (float)sx, (float)y, &gx, &gy);
            tile_key_t key;
            int32_t ax = cam.cx + (int32_t)floorf(gx), ay = cam.cy + (int32_t)floorf(gy);
            if (!view3d__key(&cam, ax >> 8, ay >> 8, &key)) continue;
            bool seen = false;
            for (size_t j = 0; j < view3d->key_count && !seen; ++j) {
                seen = view3d->keys[j].z == key.z && view3d->keys[j].x == key.x && view3d->keys[j].y == key.y;
            }
            if (!seen) view3d->keys[view3d->key_count++] = key;
        }
    }
    size_t count = 0;
    for (size_t i = 0; i < view3d->key_count && count < capacity; ++i) {
        if (tiles__needs_load(tiles, view3d->keys[i])) out_keys[count++] = view3d->keys[i];
    }
    return count;
}

bool view3d__is_near(const view3d_t *view3d, tile_key_t key) {
    for (size_t i = 0; i < view3d->key_count; ++i) {
        if (view3d->keys[i].z == key.z && view3d->keys[i].x == key.x && view3d->keys[i].y == key.y) return true;
    }
    return false;
}

/* Each row is a straight line across the ground, stepped in 16.16 fixed
 * point from the centre; the tile is looked up again only when it changes. */
static void view3d__draw_ground(const view3d_camera_t *cam, tiles_t *tiles) {
    bool epaper = display__is_epaper();
    int width = cam->width < VIEW3D_ROW_MAX ? cam->width : VIEW3D_ROW_MAX;
    int top = view3d__ground_top(cam);
    uint16_t land = epaper ? BRUCE_COLOR_WHITE : VIEW3D_LAND;
    uint16_t water = epaper ? BRUCE_COLOR_WHITE : VIEW3D_WATER;
    for (int y = top; y < cam->height; ++y) {
        float gx, gy;
        view3d__ground(cam, 0.0f, (float)y, &gx, &gy);
        float s = cam->k / ((float)y - cam->horizon);
        int32_t x_fixed = (int32_t)(gx * 65536.0f), y_fixed = (int32_t)(gy * 65536.0f);
        int32_t x_step = (int32_t)(cam->rx * s * 65536.0f), y_step = (int32_t)(cam->ry * s * 65536.0f);
        int32_t tile_x = INT32_MIN, tile_y = INT32_MIN;
        const uint16_t *pixels = NULL;
        uint16_t fill = land;
        for (int sx = 0; sx < width; ++sx) {
            int32_t ax = cam->cx + (x_fixed >> 16), ay = cam->cy + (y_fixed >> 16);
            x_fixed += x_step;
            y_fixed += y_step;
            if ((ax >> 8) != tile_x || (ay >> 8) != tile_y) {
                tile_x = ax >> 8;
                tile_y = ay >> 8;
                tile_key_t key;
                const image_bitmap_t *bitmap = NULL;
                if (view3d__key(cam, tile_x, tile_y, &key)) {
                    bitmap = tiles__get(tiles, key);
                    fill = land;
                } else {
                    fill = water;
                }
                pixels = bitmap != NULL ? bitmap->pixels : NULL;
            }
            s_row[sx] = pixels != NULL ? pixels[(ay & 255) * GEO_TILE_SIZE + (ax & 255)] : fill;
        }
        display__draw_rgb_bitmap(0, y, s_row, width, 1);
    }
}

static void view3d__draw_sky(const view3d_camera_t *cam) {
    int top = view3d__ground_top(cam);
    if (display__is_epaper()) {
        display__fill_rect(0, 0, cam->width, top, BRUCE_COLOR_WHITE);
        display__fill_rect(0, top, cam->width, 1, BRUCE_COLOR_BLACK);
        return;
    }
    int haze = top > VIEW3D_HAZE_ROWS ? VIEW3D_HAZE_ROWS : top;
    display__fill_rect(0, 0, cam->width, top - haze, VIEW3D_SKY);
    display__fill_rect(0, top - haze, cam->width, haze, VIEW3D_HAZE);
}

/* Screen position of a point given in tile pixels from the centre, and its
 * scale; false when it is behind the camera or beyond the sky line. */
static bool view3d__project(const view3d_camera_t *cam, float gx, float gy, float *sx, float *sy, float *scale) {
    float ahead = gx * cam->fx + gy * cam->fy;
    float right = gx * cam->rx + gy * cam->ry;
    float s = ahead / cam->focal + cam->s_mark;
    if (s <= cam->s_mark * 0.05f || s > cam->s_max) return false;
    *sx = cam->width / 2.0f + right / s;
    *sy = cam->horizon + cam->k / s;
    if (scale != NULL) *scale = s;
    return true;
}

static float view3d__ahead(const view3d_camera_t *cam, float gx, float gy) { return gx * cam->fx + gy * cam->fy; }

/* A line on the ground, cut to the part the camera sees, wider when near. */
static void view3d__ground_line(const view3d_camera_t *cam, float x0, float y0, float x1, float y1, float base_width,
                                uint16_t color) {
    float near = cam->focal * (cam->s_mark * 0.06f - cam->s_mark);
    float far = cam->focal * (cam->s_max - cam->s_mark) * 0.999f;
    float a0 = view3d__ahead(cam, x0, y0), a1 = view3d__ahead(cam, x1, y1);
    if ((a0 < near && a1 < near) || (a0 > far && a1 > far)) return;
    float t0 = 0, t1 = 1;
    if (a0 != a1) {
        float tn = (near - a0) / (a1 - a0), tf = (far - a0) / (a1 - a0);
        if (a0 < near) t0 = tn;
        if (a1 < near) t1 = tn;
        if (a0 > far) t0 = tf;
        if (a1 > far) t1 = tf;
    }
    float px0 = x0 + (x1 - x0) * t0, py0 = y0 + (y1 - y0) * t0;
    float px1 = x0 + (x1 - x0) * t1, py1 = y0 + (y1 - y0) * t1;
    float sx0, sy0, sx1, sy1, s0, s1;
    if (!view3d__project(cam, px0, py0, &sx0, &sy0, &s0) || !view3d__project(cam, px1, py1, &sx1, &sy1, &s1)) return;
    float half = base_width * cam->s_mark * 2.0f / (s0 + s1) / 2.0f;
    if (half < 0.5f) half = 0.5f;
    if (half > 6.0f) half = 6.0f;
    float dx = sx1 - sx0, dy = sy1 - sy0, length = sqrtf(dx * dx + dy * dy);
    if (length < 0.5f) return;
    float nx = -dy / length * half, ny = dx / length * half;
    display__fill_triangle((int16_t)(sx0 + nx), (int16_t)(sy0 + ny), (int16_t)(sx0 - nx), (int16_t)(sy0 - ny),
                           (int16_t)(sx1 + nx), (int16_t)(sy1 + ny), color);
    display__fill_triangle((int16_t)(sx0 - nx), (int16_t)(sy0 - ny), (int16_t)(sx1 - nx), (int16_t)(sy1 - ny),
                           (int16_t)(sx1 + nx), (int16_t)(sy1 + ny), color);
    display__fill_circle((int16_t)sx1, (int16_t)sy1, (int16_t)half, color);
}

static void view3d__draw_route(const view3d_camera_t *cam, const route_t *route) {
    float scale = 1.0f / (float)(1 << (ROUTE_ZOOM - cam->zoom));
    uint16_t color = display__is_epaper() ? BRUCE_COLOR_BLACK : VIEW3D_ROUTE;
    float px = route->points[0].x * scale - cam->cx, py = route->points[0].y * scale - cam->cy;
    for (size_t i = 1; i < route->point_count; ++i) {
        float x = route->points[i].x * scale - cam->cx, y = route->points[i].y * scale - cam->cy;
        if (fabsf(x - px) < 1 && fabsf(y - py) < 1 && i + 1 < route->point_count) continue;
        view3d__ground_line(cam, px, py, x, y, 5.0f, color);
        px = x;
        py = y;
    }
}

static bool view3d__project_lat_lon(const view3d_camera_t *cam, double lat, double lon, float *sx, float *sy) {
    geo_point_t point = geo__from_lat_lon(lat, lon, cam->zoom);
    return view3d__project(cam, (float)(point.x - cam->cx), (float)(point.y - cam->cy), sx, sy, NULL);
}

/* A pin standing up from the ground. */
static void view3d__draw_pin(const view3d_camera_t *cam, double lat, double lon) {
    float sx, sy;
    if (!view3d__project_lat_lon(cam, lat, lon, &sx, &sy)) return;
    bool epaper = display__is_epaper();
    uint16_t color = epaper ? BRUCE_COLOR_BLACK : VIEW3D_PIN;
    display__fill_rect((int16_t)sx - 1, (int16_t)sy - 14, 3, 14, color);
    display__fill_circle((int16_t)sx, (int16_t)sy - 16, 6, color);
    display__fill_circle((int16_t)sx, (int16_t)sy - 16, 2, BRUCE_COLOR_WHITE);
}

/* An arrow pointing the way the view faces. */
static void view3d__draw_me(const view3d_camera_t *cam, double lat, double lon) {
    float sx, sy;
    if (!view3d__project_lat_lon(cam, lat, lon, &sx, &sy)) return;
    bool epaper = display__is_epaper();
    int x = (int)sx, y = (int)sy;
    display__fill_triangle(x, y - 11, x - 9, y + 8, x + 9, y + 8, BRUCE_COLOR_WHITE);
    display__fill_triangle(x, y - 8, x - 6, y + 5, x + 6, y + 5, epaper ? BRUCE_COLOR_BLACK : VIEW3D_ME);
}

static void view3d__draw_crosshair(const view3d_camera_t *cam) {
    int x = cam->width / 2, y = (int)cam->marker_y;
    uint16_t color = display__is_epaper() ? BRUCE_COLOR_BLACK : 0x2104;
    display__fill_rect(x - 6, y, 13, 1, color);
    display__fill_rect(x, y - 6, 1, 13, color);
}

void view3d__draw(const view3d_t *view3d, const map_view_t *view, tiles_t *tiles, const hud_state_t *hud, int width,
                  int height) {
    view3d_camera_t cam = view3d__camera(view3d, view, width, height);
    view3d__draw_sky(&cam);
    view3d__draw_ground(&cam, tiles);
    if (hud->route != NULL) view3d__draw_route(&cam, hud->route);
    if (hud->pin) view3d__draw_pin(&cam, hud->pin_lat, hud->pin_lon);
    if (!hud->following) view3d__draw_crosshair(&cam);
    if (hud->me) view3d__draw_me(&cam, hud->me_lat, hud->me_lon);
}
