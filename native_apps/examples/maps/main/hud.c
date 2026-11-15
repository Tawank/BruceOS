#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/display.h"

#define HUD_MARGIN 4
#define HUD_PAD 4
#define HUD_PANEL 0x2124
#define HUD_TEXT BRUCE_COLOR_WHITE
#define HUD_MUTED 0xAD55
#define HUD_ACCENT 0xFB20
#define HUD_INK 0x2104
#define HUD_SCALE_MAX_PX 70
#define HUD_CROSS 6

static int16_t s_char_w = 6;
static int16_t s_char_h = 8;

static int hud__text_width(const char *text) { return (int)strlen(text) * s_char_w; }

static int hud__pill_height(void) { return s_char_h + 2 * HUD_PAD; }

static void hud__pill(const char *text, int x, int y, bruce_display_color_t color) {
    int w = hud__text_width(text) + 2 * HUD_PAD;
    int h = hud__pill_height();
    display__fill_round_rect(x, y, w, h, h / 2, HUD_PANEL);
    display__set_text_color(color);
    display__draw_string(text, x + HUD_PAD, y + HUD_PAD);
}

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

static int hud__draw_coordinates(const map_view_t *view) {
    double lat = 0, lon = 0;
    char lat_text[16], lon_text[16], text[40];
    map_view__center(view, &lat, &lon);
    hud__format_degrees(lat_text, sizeof(lat_text), lat);
    hud__format_degrees(lon_text, sizeof(lon_text), lon);
    snprintf(text, sizeof(text), "%s, %s", lat_text, lon_text);
    hud__pill(text, HUD_MARGIN, HUD_MARGIN, HUD_TEXT);
    return HUD_MARGIN + hud__text_width(text) + 2 * HUD_PAD;
}

/* Next to the coordinates when it fits, under them on narrow screens. Returns the row it used. */
static int hud__draw_zoom(const map_view_t *view, hud_wheel_mode_t mode, int width, int coordinates_end) {
    static const char *const mode_suffix[] = {"", " <>", " ^v"};
    char text[12];
    snprintf(text, sizeof(text), "z%d%s", view->zoom, mode_suffix[mode]);
    int x = width - HUD_MARGIN - hud__text_width(text) - 2 * HUD_PAD;
    int row = x > coordinates_end + HUD_MARGIN ? 0 : 1;
    hud__pill(text, x, HUD_MARGIN + row * (hud__pill_height() + 2), HUD_TEXT);
    return row;
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

void hud__draw(const map_view_t *view, const hud_state_t *state, int width, int height) {
    display__set_text_size(1);
    display__get_font_metrics(&s_char_w, &s_char_h);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);

    hud__draw_crosshair(width / 2, height / 2);
    int coordinates_end = hud__draw_coordinates(view);
    int zoom_row = hud__draw_zoom(view, state->wheel_mode, width, coordinates_end);
    hud__draw_scale(view, height);
    hud__draw_attribution(width, height);
    if (state->status != NULL) {
        int row = 1 + zoom_row;
        hud__pill(state->status, HUD_MARGIN, HUD_MARGIN + row * (hud__pill_height() + 2), HUD_ACCENT);
    }
}
