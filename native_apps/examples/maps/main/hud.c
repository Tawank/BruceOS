#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/display.h"
#include "core_sdk/icon.h"

#define HUD_MARGIN 4
#define HUD_PAD 4
#define HUD_GAP 3
#define HUD_PANEL 0x2124
#define HUD_TEXT BRUCE_COLOR_WHITE
#define HUD_MUTED 0xAD55
#define HUD_ACCENT 0xFB20
#define HUD_INK 0x2104
#define HUD_SCALE_MAX_PX 70
#define HUD_CROSS 6
#define HUD_BUTTON_SIZE 28
#define HUD_BUTTON_ICON 20
#define HUD_PIN_SIZE 24
#define HUD_ME 0x1C9F
#define HUD_ME_RADIUS 5
#define HUD_ROUTE 0x2B9F
#define HUD_ROUTE_WIDTH 5
#define HUD_ARROW_WIDTH 3

static int16_t s_char_w = 6;
static int16_t s_char_h = 8;

const char *hud__nav_icon(hud_nav_mode_t mode) {
    static const char *const icons[HUD_NAV_COUNT] = {"plus", "arrow-up-down", "arrow-left-right"};
    return mode < HUD_NAV_COUNT ? icons[mode] : NULL;
}

/* Counts characters, not UTF-8 bytes. */
static int hud__text_width(const char *text) {
    int chars = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) chars += (*c & 0xC0) != 0x80;
    return chars * s_char_w;
}

static int hud__pill_height(void) { return s_char_h + 2 * HUD_PAD; }

static int hud__icon_size(void) { return s_char_h + 4; }

static void hud__icon(const char *name, int x, int y, int size, bruce_display_color_t color) {
    const bruce_icon_t *icon = name != NULL ? icon__get(name) : NULL;
    if (icon == NULL) return;
    display__draw_bitmap_scaled(x, y, icon->bits, icon->width, icon->height, size, size, color);
}

static int hud__pill_width(const char *text, const char *icon) {
    int w = hud__text_width(text) + 2 * HUD_PAD;
    return icon != NULL ? w + hud__icon_size() + HUD_GAP : w;
}

/* Rounded label with an optional leading icon. */
static void hud__pill(const char *text, const char *icon, int x, int y, bruce_display_color_t color) {
    int h = hud__pill_height();
    display__fill_round_rect(x, y, hud__pill_width(text, icon), h, h / 2, HUD_PANEL);
    int text_x = x + HUD_PAD;
    if (icon != NULL) {
        hud__icon(icon, text_x, y + (h - hud__icon_size()) / 2, hud__icon_size(), color);
        text_x += hud__icon_size() + HUD_GAP;
    }
    display__set_text_color(color);
    display__draw_string(text, text_x, y + HUD_PAD);
}

static int hud__row_y(int row) { return HUD_MARGIN + row * (hud__pill_height() + 2); }

static void hud__format_degrees(char *out, size_t capacity, double value) {
    long scaled = (long)floor(fabs(value) * 100000.0 + 0.5);
    snprintf(out, capacity, "%s%ld.%05ld", value < 0 ? "-" : "", scaled / 100000, scaled % 100000);
}

static void hud__draw_crosshair(int cx, int cy) {
    display__fill_rect(cx - HUD_CROSS - 1, cy - 1, 2 * HUD_CROSS + 3, 3, BRUCE_COLOR_WHITE);
    display__fill_rect(cx - 1, cy - HUD_CROSS - 1, 3, 2 * HUD_CROSS + 3, BRUCE_COLOR_WHITE);
    display__fill_rect(cx - HUD_CROSS, cy, 2 * HUD_CROSS + 1, 1, HUD_INK);
    display__fill_rect(cx, cy - HUD_CROSS, 1, 2 * HUD_CROSS + 1, HUD_INK);
    display__fill_circle(cx, cy, 2, HUD_ACCENT);
}

/* The marker's tip touches the place; a dark halo keeps it visible on any map colour. */
static void hud__draw_pin(const map_view_t *view, double lat, double lon, int width, int height) {
    geo_point_t point = geo__from_lat_lon(lat, lon, view->zoom);
    int x = point.x - view->center.x + width / 2 - HUD_PIN_SIZE / 2;
    int y = point.y - view->center.y + height / 2 - HUD_PIN_SIZE + 2;
    if (x < -HUD_PIN_SIZE || x > width || y < -HUD_PIN_SIZE || y > height) return;
    static const int8_t halo[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (size_t i = 0; i < sizeof(halo) / sizeof(halo[0]); ++i) {
        hud__icon("map-marker", x + halo[i][0], y + halo[i][1], HUD_PIN_SIZE, HUD_INK);
    }
    hud__icon("map-marker", x, y, HUD_PIN_SIZE, HUD_ACCENT);
}

/* A blue dot in a white ring, outlined so it shows on any map colour. */
static void hud__draw_me(const map_view_t *view, double lat, double lon, int width, int height) {
    geo_point_t point = geo__from_lat_lon(lat, lon, view->zoom);
    int x = point.x - view->center.x + width / 2;
    int y = point.y - view->center.y + height / 2;
    int r = HUD_ME_RADIUS + 3;
    if (x < -r || x > width + r || y < -r || y > height + r) return;
    display__fill_circle(x, y, HUD_ME_RADIUS + 3, HUD_INK);
    display__fill_circle(x, y, HUD_ME_RADIUS + 2, BRUCE_COLOR_WHITE);
    display__fill_circle(x, y, HUD_ME_RADIUS, HUD_ME);
}

static int hud__round(double value) { return (int)floor(value + 0.5); }

/* A line `thickness` pixels wide, as a square brush. */
static void hud__thick_line(int x0, int y0, int x1, int y1, int thickness, bruce_display_color_t color) {
    int from = -(thickness / 2), to = from + thickness;
    for (int dy = from; dy < to; ++dy) {
        for (int dx = from; dx < to; ++dx) {
            display__draw_line((int16_t)(x0 + dx), (int16_t)(y0 + dy), (int16_t)(x1 + dx), (int16_t)(y1 + dy), color);
        }
    }
}

/* Liang-Barsky: cuts the segment to the box, false when it misses it. */
static bool hud__clip(double *x0, double *y0, double *x1, double *y1, double left, double top, double right,
                      double bottom) {
    double t0 = 0, t1 = 1, dx = *x1 - *x0, dy = *y1 - *y0;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {*x0 - left, right - *x0, *y0 - top, bottom - *y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        double t = q[i] / p[i];
        if (p[i] < 0 && t > t0) t0 = t;
        if (p[i] > 0 && t < t1) t1 = t;
        if (t0 > t1) return false;
    }
    double sx = *x0, sy = *y0;
    *x0 = sx + t0 * dx;
    *y0 = sy + t0 * dy;
    *x1 = sx + t1 * dx;
    *y1 = sy + t1 * dy;
    return true;
}

/* Blue on colour screens; black on e-paper, where it stands out from the outlined map. */
static void hud__draw_route(const map_view_t *view, const route_t *route, int width, int height) {
    int shift = ROUTE_ZOOM - view->zoom;
    double scale = shift >= 0 ? 1.0 / (double)(1 << shift) : (double)(1 << -shift);
    double left = view->center.x - width / 2.0, top = view->center.y - height / 2.0;
    int margin = HUD_ROUTE_WIDTH;
    bool epaper = display__is_epaper();
    bruce_display_color_t color = epaper ? BRUCE_COLOR_BLACK : HUD_ROUTE;
    int thickness = epaper ? HUD_ROUTE_WIDTH - 1 : HUD_ROUTE_WIDTH;
    double px = route->points[0].x * scale - left, py = route->points[0].y * scale - top;
    for (size_t i = 1; i < route->point_count; ++i) {
        double x = route->points[i].x * scale - left, y = route->points[i].y * scale - top;
        /* Points closer than a pixel add nothing when zoomed out. */
        if (fabs(x - px) < 1 && fabs(y - py) < 1 && i + 1 < route->point_count) continue;
        double x0 = px, y0 = py, x1 = x, y1 = y;
        if (hud__clip(&x0, &y0, &x1, &y1, -margin, -margin, width + margin, height + margin)) {
            hud__thick_line(hud__round(x0), hud__round(y0), hud__round(x1), hud__round(y1), thickness, color);
        }
        px = x;
        py = y;
    }
}

/* An arrowhead at (x, y) pointing along (dx, dy), a unit vector. */
static void hud__arrowhead(double x, double y, double dx, double dy, int size, bruce_display_color_t color) {
    double bx = x - dx * size, by = y - dy * size;
    double nx = -dy * size * 0.7, ny = dx * size * 0.7;
    display__fill_triangle((int16_t)hud__round(x + dx * 2), (int16_t)hud__round(y + dy * 2), (int16_t)hud__round(bx + nx),
                           (int16_t)hud__round(by + ny), (int16_t)hud__round(bx - nx), (int16_t)hud__round(by - ny), color);
}

/* The manoeuvre as a drawn arrow in a `size` square: a stem coming up from
 * the bottom that bends by the turn's angle, a ring for roundabouts, a target
 * for the destination. */
static void hud__draw_maneuver(const route_step_t *step, int x, int y, int size, bruce_display_color_t color) {
    double cx = x + size / 2.0, cy = y + size / 2.0;
    int head = size / 4;
    if (step->turn == ROUTE_ARRIVE) {
        display__fill_circle((int16_t)cx, (int16_t)cy, (int16_t)(size / 3), color);
        display__fill_circle((int16_t)cx, (int16_t)cy, (int16_t)(size / 3 - HUD_ARROW_WIDTH), HUD_PANEL);
        display__fill_circle((int16_t)cx, (int16_t)cy, (int16_t)(size / 8), color);
        return;
    }
    if (step->angle == 180) {
        double right = cx + size / 6.0, leftx = cx - size / 5.0, top = y + size / 4.0;
        hud__thick_line((int)right, y + size - 2, (int)right, (int)top, HUD_ARROW_WIDTH, color);
        hud__thick_line((int)right, (int)top, (int)leftx, (int)top, HUD_ARROW_WIDTH, color);
        hud__thick_line((int)leftx, (int)top, (int)leftx, (int)(cy + size / 8.0), HUD_ARROW_WIDTH, color);
        hud__arrowhead(leftx, cy + size / 8.0 + head, 0, 1, head, color);
        return;
    }
    double rad = step->angle * M_PI / 180.0;
    double dx = sin(rad), dy = -cos(rad);
    if (step->turn == ROUTE_ROUNDABOUT) {
        int r = size / 5;
        display__fill_circle((int16_t)cx, (int16_t)cy, (int16_t)r, color);
        display__fill_circle((int16_t)cx, (int16_t)cy, (int16_t)(r - HUD_ARROW_WIDTH + 1), HUD_PANEL);
        hud__thick_line((int)cx, y + size - 2, (int)cx, (int)(cy + r), HUD_ARROW_WIDTH, color);
        double ex = cx + dx * r, ey = cy + dy * r;
        double tip_x = cx + dx * (size / 2.0 - 2), tip_y = cy + dy * (size / 2.0 - 2);
        hud__thick_line((int)ex, (int)ey, (int)(tip_x - dx * head), (int)(tip_y - dy * head), HUD_ARROW_WIDTH, color);
        hud__arrowhead(tip_x, tip_y, dx, dy, head, color);
        return;
    }
    double bend_y = step->angle == 0 ? y + size / 4.0 : cy + size / 8.0;
    hud__thick_line((int)cx, y + size - 2, (int)cx, (int)bend_y, HUD_ARROW_WIDTH, color);
    double length = step->angle == 0 ? 0 : size / 2.5;
    double tip_x = cx + dx * (length + head), tip_y = bend_y + dy * (length + head);
    if (length > 0) hud__thick_line((int)cx, (int)bend_y, (int)(cx + dx * length), (int)(bend_y + dy * length),
                                    HUD_ARROW_WIDTH, color);
    hud__arrowhead(tip_x, tip_y, dx, dy, head, color);
}

static void hud__draw_coordinates(const map_view_t *view) {
    double lat = 0, lon = 0;
    char lat_text[16], lon_text[16], text[40];
    map_view__center(view, &lat, &lon);
    hud__format_degrees(lat_text, sizeof(lat_text), lat);
    hud__format_degrees(lon_text, sizeof(lon_text), lon);
    snprintf(text, sizeof(text), "%s, %s", lat_text, lon_text);
    hud__pill(text, NULL, HUD_MARGIN, hud__row_y(1), HUD_TEXT);
}

static void hud__zoom_text(const map_view_t *view, hud_nav_mode_t mode, char *text, size_t capacity,
                           const char **icon) {
    snprintf(text, capacity, "z%d", view->zoom);
    *icon = mode == HUD_NAV_ZOOM ? NULL : hud__nav_icon(mode);
}

/* Zoom level, plus the pan direction Prev/Next move in, top right. */
static void hud__draw_zoom(const map_view_t *view, hud_nav_mode_t mode, int width) {
    char text[8];
    const char *icon = NULL;
    hud__zoom_text(view, mode, text, sizeof(text), &icon);
    hud__pill(text, icon, width - HUD_MARGIN - hud__pill_width(text, icon), HUD_MARGIN, HUD_TEXT);
}

/* Fills the top row up to the zoom badge. */
hud_rect_t hud__search_rect(const map_view_t *view, const hud_state_t *state, int width) {
    char text[8];
    const char *icon = NULL;
    hud__zoom_text(view, state->nav_mode, text, sizeof(text), &icon);
    int w = width - 2 * HUD_MARGIN - HUD_GAP - hud__pill_width(text, icon);
    return (hud_rect_t){.x = HUD_MARGIN, .y = HUD_MARGIN, .w = w, .h = hud__pill_height()};
}

/* The first `len` bytes of text, cut with "..." when wider than `max_width`. */
static void hud__fit(const char *text, size_t len, int max_width, char *out, size_t capacity) {
    snprintf(out, capacity, "%.*s", (int)len, text);
    if (hud__text_width(out) <= max_width) return;
    int keep = max_width / s_char_w - 3;
    size_t bytes = 0;
    for (int chars = 0; out[bytes] != '\0'; ++bytes) {
        if (((unsigned char)out[bytes] & 0xC0) != 0x80 && chars++ == keep) break;
    }
    snprintf(out + bytes, capacity - bytes, "...");
}

/* The name up to its first comma. */
static void hud__fit_name(const char *name, int max_width, char *out, size_t capacity) {
    hud__fit(name, strcspn(name, ","), max_width, out, capacity);
}

/* Splits at the last space that lets the first line fit; the rest is cut to one more line. */
static void hud__wrap(const char *text, int max_width, char *first, char *second, size_t capacity) {
    second[0] = '\0';
    if (hud__text_width(text) <= max_width) {
        snprintf(first, capacity, "%s", text);
        return;
    }
    size_t split = 0;
    int chars = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (((unsigned char)text[i] & 0xC0) == 0x80) continue;
        if (text[i] == ' ' && chars * s_char_w <= max_width) split = i;
        chars++;
    }
    if (split == 0) {
        hud__fit(text, strlen(text), max_width, first, capacity);
        return;
    }
    snprintf(first, capacity, "%.*s", (int)split, text);
    hud__fit(text + split + 1, strlen(text + split + 1), max_width, second, capacity);
}

static int hud__banner_height(void) { return 2 * HUD_PAD + 2 * s_char_h + 2 + 2 * s_char_h + 1; }

hud_rect_t hud__banner_rect(int width) {
    return (hud_rect_t){.x = HUD_MARGIN, .y = HUD_MARGIN, .w = width - 2 * HUD_MARGIN, .h = hud__banner_height()};
}

/* The next manoeuvre: arrow, distance to it, and the instruction on up to two lines. Returns its bottom. */
static int hud__draw_banner(const hud_state_t *state, int width) {
    hud_rect_t box = hud__banner_rect(width);
    display__fill_round_rect(box.x, box.y, box.w, box.h, 6, HUD_PANEL);
    int arrow = box.h - 2 * HUD_PAD;
    hud__draw_maneuver(state->banner_step, box.x + HUD_PAD, box.y + HUD_PAD, arrow, HUD_TEXT);
    int text_x = box.x + HUD_PAD + arrow + HUD_GAP + 2;
    int text_w = box.x + box.w - HUD_PAD - text_x;
    int y = box.y + HUD_PAD;
    if (state->banner_distance != NULL) {
        display__set_text_color(HUD_TEXT);
        display__set_text_size(2);
        display__draw_string(state->banner_distance, text_x, y);
        display__set_text_size(1);
    }
    y += 2 * s_char_h + 2;
    char lines[2][96];
    hud__wrap(state->banner_text != NULL ? state->banner_text : "", text_w, lines[0], lines[1], sizeof(lines[0]));
    display__set_text_color(HUD_TEXT);
    display__draw_string(lines[0], text_x, y);
    display__draw_string(lines[1], text_x, y + s_char_h + 1);
    int bottom = box.y + box.h + 2;
    if (state->banner_then != NULL) {
        char then[96], text[104];
        snprintf(text, sizeof(text), "Then %s", state->banner_then);
        if (text[5] >= 'A' && text[5] <= 'Z') text[5] = (char)(text[5] - 'A' + 'a');
        hud__fit(text, strlen(text), width - 2 * HUD_MARGIN - 2 * HUD_PAD, then, sizeof(then));
        hud__pill(then, NULL, HUD_MARGIN, bottom, HUD_MUTED);
        bottom += hud__pill_height() + 2;
    }
    return bottom;
}

/* Bottom centre, above the scale bar and attribution. */
static void hud__draw_summary(const char *summary, int width, int height) {
    int y = height - HUD_MARGIN - s_char_h - 6 - hud__pill_height();
    hud__pill(summary, NULL, (width - hud__pill_width(summary, NULL)) / 2, y, HUD_TEXT);
}

static void hud__draw_search(const map_view_t *view, const hud_state_t *state, int width) {
    hud_rect_t box = hud__search_rect(view, state, width);
    display__fill_round_rect(box.x, box.y, box.w, box.h, box.h / 2, HUD_PANEL);
    int icon_x = box.x + HUD_PAD + 1;
    hud__icon("magnify", icon_x, box.y + (box.h - hud__icon_size()) / 2, hud__icon_size(), HUD_TEXT);
    int text_x = icon_x + hud__icon_size() + HUD_GAP;
    char text[64];
    if (state->place != NULL) hud__fit_name(state->place, box.x + box.w - HUD_PAD - text_x, text, sizeof(text));
    display__set_text_color(state->place != NULL ? HUD_TEXT : HUD_MUTED);
    display__draw_string(state->place != NULL ? text : "Search", text_x, box.y + HUD_PAD);
}

static void hud__draw_scale(const map_view_t *view, int height) {
    static const long steps_m[] = {
        5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000,
        50000, 100000, 200000, 500000, 1000000, 2000000, 5000000,
    };
    double lat = 0, lon = 0;
    map_view__center(view, &lat, &lon);
    double meters_per_px = geo__meters_per_pixel(lat, view->zoom);

    long meters = steps_m[0];
    for (size_t i = 0; i < sizeof(steps_m) / sizeof(steps_m[0]); ++i) {
        if (steps_m[i] / meters_per_px > HUD_SCALE_MAX_PX) break;
        meters = steps_m[i];
    }
    int bar = (int)(meters / meters_per_px);
    char label[24];
    if (meters >= 1000) snprintf(label, sizeof(label), "%ld km", meters / 1000);
    else snprintf(label, sizeof(label), "%ld m", meters);

    int x = HUD_MARGIN;
    int y = height - HUD_MARGIN - s_char_h - 6;
    display__set_text_color(HUD_INK);
    display__draw_string(label, x, y);
    display__fill_rect(x, y + s_char_h + 2, bar + 2, 4, BRUCE_COLOR_WHITE);
    display__fill_rect(x + 1, y + s_char_h + 3, bar, 2, HUD_INK);
}

static void hud__draw_attribution(int width, int height) {
    const char *text = "(c) OpenStreetMap";
    display__set_text_color(HUD_MUTED);
    display__draw_string(text, width - HUD_MARGIN - hud__text_width(text), height - HUD_MARGIN - s_char_h);
}

/* My location sits in the bottom right corner; +/- are centred on the right edge, above it when short. */
hud_rect_t hud__button_rect(hud_button_t button, int width, int height) {
    hud_rect_t rect = {.x = width - HUD_MARGIN - HUD_BUTTON_SIZE, .w = HUD_BUTTON_SIZE, .h = HUD_BUTTON_SIZE};
    int locate_y = height - HUD_MARGIN - s_char_h - HUD_GAP - HUD_BUTTON_SIZE;
    int out_y = height / 2 + 2;
    if (out_y > locate_y - HUD_GAP - HUD_BUTTON_SIZE) out_y = locate_y - HUD_GAP - HUD_BUTTON_SIZE;
    rect.y = button == HUD_BUTTON_LOCATE    ? locate_y
             : button == HUD_BUTTON_ZOOM_OUT ? out_y
                                             : out_y - 4 - HUD_BUTTON_SIZE;
    return rect;
}

static void hud__draw_button(hud_button_t button, const char *icon, bruce_display_color_t color, int width,
                             int height) {
    const int inset = (HUD_BUTTON_SIZE - HUD_BUTTON_ICON) / 2;
    hud_rect_t rect = hud__button_rect(button, width, height);
    display__fill_round_rect(rect.x, rect.y, rect.w, rect.h, 6, HUD_PANEL);
    hud__icon(icon, rect.x + inset, rect.y + inset, HUD_BUTTON_ICON, color);
}

/* Filled while there is a fix, orange while the map follows it. */
static void hud__draw_locate(const hud_state_t *state, int width, int height) {
    const char *icon = state->me ? "crosshairs-gps" : "crosshairs";
    bruce_display_color_t color = state->following ? HUD_ACCENT : state->me ? HUD_TEXT : HUD_MUTED;
    hud__draw_button(HUD_BUTTON_LOCATE, icon, color, width, height);
}

/* Big number in a box at the left, above the scale bar, clear of the hint and the buttons. */
static void hud__draw_speed(int speed_kmh, int height) {
    char number[8];
    snprintf(number, sizeof(number), "%d", speed_kmh);
    const char *unit = "km/h";
    int number_w = (int)strlen(number) * s_char_w * 2;
    int unit_w = hud__text_width(unit);
    int w = (number_w > unit_w ? number_w : unit_w) + 2 * HUD_PAD;
    int h = 3 * s_char_h + 2 * HUD_PAD;
    int x = HUD_MARGIN;
    int y = height - HUD_MARGIN - s_char_h - 8 - h;
    display__fill_round_rect(x, y, w, h, 6, HUD_PANEL);
    display__set_text_color(HUD_TEXT);
    display__set_text_size(2);
    display__draw_string(number, x + (w - number_w) / 2, y + HUD_PAD);
    display__set_text_size(1);
    display__set_text_color(HUD_MUTED);
    display__draw_string(unit, x + (w - unit_w) / 2, y + HUD_PAD + 2 * s_char_h);
}

/* Sits on the bottom edge, over the scale bar and attribution while it shows. */
static void hud__draw_hint(const char *const hint[2], const char *icon, int width, int height) {
    int lines = hint[0] == NULL ? 0 : hint[1] == NULL ? 1 : 2;
    int y = height - HUD_MARGIN - lines * (hud__pill_height() + 2) + 2;
    for (int i = 0; i < lines; ++i) {
        const char *line_icon = i == 0 ? icon : NULL;
        int x = (width - hud__pill_width(hint[i], line_icon)) / 2;
        hud__pill(hint[i], line_icon, x, y, i == 0 ? HUD_TEXT : HUD_MUTED);
        y += hud__pill_height() + 2;
    }
}

void hud__draw(const map_view_t *view, const hud_state_t *state, int width, int height) {
    display__set_text_size(1);
    display__get_font_metrics(&s_char_w, &s_char_h);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);

    if (state->route != NULL) hud__draw_route(view, state->route, width, height);
    if (state->pin) hud__draw_pin(view, state->pin_lat, state->pin_lon, width, height);
    if (!state->following) hud__draw_crosshair(width / 2, height / 2);
    if (state->me) hud__draw_me(view, state->me_lat, state->me_lon, width, height);
    int top = hud__row_y(2);
    if (state->banner_step != NULL) {
        top = hud__draw_banner(state, width);
    } else {
        hud__draw_search(view, state, width);
        hud__draw_zoom(view, state->nav_mode, width);
        hud__draw_coordinates(view);
    }
    hud__draw_scale(view, height);
    hud__draw_attribution(width, height);
    if (state->zoom_buttons) {
        hud__draw_button(HUD_BUTTON_ZOOM_IN, "plus", HUD_TEXT, width, height);
        hud__draw_button(HUD_BUTTON_ZOOM_OUT, "minus", HUD_TEXT, width, height);
    }
    if (state->locate_button) hud__draw_locate(state, width, height);
    if (state->status != NULL) {
        hud__pill(state->status, state->status_icon, HUD_MARGIN, top, HUD_ACCENT);
        top += hud__pill_height() + 2;
    }
    if (state->gps_status != NULL) hud__pill(state->gps_status, "crosshairs", HUD_MARGIN, top, HUD_TEXT);
    if (state->speed) hud__draw_speed(state->speed_kmh, height);
    if (state->summary != NULL) hud__draw_summary(state->summary, width, height);
    hud__draw_hint(state->hint, state->hint_icon, width, height);
}
