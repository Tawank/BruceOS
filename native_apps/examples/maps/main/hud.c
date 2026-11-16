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

static int16_t s_char_w = 6;
static int16_t s_char_h = 8;

const char *hud__nav_icon(hud_nav_mode_t mode) {
    static const char *const icons[HUD_NAV_COUNT] = {"plus", "arrow-up-down", "arrow-left-right"};
    return mode < HUD_NAV_COUNT ? icons[mode] : NULL;
}

static int hud__text_width(const char *text) { return (int)strlen(text) * s_char_w; }

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

static int hud__draw_coordinates(const map_view_t *view) {
    double lat = 0, lon = 0;
    char lat_text[16], lon_text[16], text[40];
    map_view__center(view, &lat, &lon);
    hud__format_degrees(lat_text, sizeof(lat_text), lat);
    hud__format_degrees(lon_text, sizeof(lon_text), lon);
    snprintf(text, sizeof(text), "%s, %s", lat_text, lon_text);
    hud__pill(text, NULL, HUD_MARGIN, HUD_MARGIN, HUD_TEXT);
    return HUD_MARGIN + hud__pill_width(text, NULL);
}

/* Zoom level, plus the pan direction Prev/Next move in. Next to the
 * coordinates when it fits, under them on narrow screens. Returns the row. */
static int hud__draw_zoom(const map_view_t *view, hud_nav_mode_t mode, int width, int coordinates_end) {
    char text[8];
    snprintf(text, sizeof(text), "z%d", view->zoom);
    const char *icon = mode == HUD_NAV_ZOOM ? NULL : hud__nav_icon(mode);
    int x = width - HUD_MARGIN - hud__pill_width(text, icon);
    int row = x > coordinates_end + HUD_MARGIN ? 0 : 1;
    hud__pill(text, icon, x, hud__row_y(row), HUD_TEXT);
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

hud_rect_t hud__button_rect(hud_button_t button, int width, int height) {
    int y = height / 2 + (button == HUD_BUTTON_ZOOM_IN ? -HUD_BUTTON_SIZE - 2 : 2);
    return (hud_rect_t){.x = width - HUD_MARGIN - HUD_BUTTON_SIZE, .y = y, .size = HUD_BUTTON_SIZE};
}

static void hud__draw_buttons(int width, int height) {
    static const char *const icons[] = {[HUD_BUTTON_ZOOM_IN] = "plus", [HUD_BUTTON_ZOOM_OUT] = "minus"};
    const int inset = (HUD_BUTTON_SIZE - HUD_BUTTON_ICON) / 2;
    for (int button = HUD_BUTTON_ZOOM_IN; button <= HUD_BUTTON_ZOOM_OUT; ++button) {
        hud_rect_t rect = hud__button_rect(button, width, height);
        display__fill_round_rect(rect.x, rect.y, rect.size, rect.size, 6, HUD_PANEL);
        hud__icon(icons[button], rect.x + inset, rect.y + inset, HUD_BUTTON_ICON, HUD_TEXT);
    }
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

    hud__draw_crosshair(width / 2, height / 2);
    int coordinates_end = hud__draw_coordinates(view);
    int zoom_row = hud__draw_zoom(view, state->nav_mode, width, coordinates_end);
    hud__draw_scale(view, height);
    hud__draw_attribution(width, height);
    if (state->zoom_buttons) hud__draw_buttons(width, height);
    if (state->status != NULL) {
        hud__pill(state->status, state->status_icon, HUD_MARGIN, hud__row_y(1 + zoom_row), HUD_ACCENT);
    }
    hud__draw_hint(state->hint, hud__nav_icon(state->nav_mode), width, height);
}
