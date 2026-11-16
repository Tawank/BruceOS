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

/* The name up to its first comma, cut with "..." when wider than `max_width`. */
static void hud__fit_name(const char *name, int max_width, char *out, size_t capacity) {
    size_t len = strcspn(name, ",");
    snprintf(out, capacity, "%.*s", (int)len, name);
    if (hud__text_width(out) <= max_width) return;
    int keep = max_width / s_char_w - 3;
    size_t bytes = 0;
    for (int chars = 0; out[bytes] != '\0'; ++bytes) {
        if (((unsigned char)out[bytes] & 0xC0) != 0x80 && chars++ == keep) break;
    }
    snprintf(out + bytes, capacity - bytes, "...");
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

hud_rect_t hud__button_rect(hud_button_t button, int width, int height) {
    int y = height / 2 + (button == HUD_BUTTON_ZOOM_IN ? -HUD_BUTTON_SIZE - 2 : 2);
    return (hud_rect_t){.x = width - HUD_MARGIN - HUD_BUTTON_SIZE, .y = y, .w = HUD_BUTTON_SIZE, .h = HUD_BUTTON_SIZE};
}

static void hud__draw_buttons(int width, int height) {
    static const char *const icons[] = {[HUD_BUTTON_ZOOM_IN] = "plus", [HUD_BUTTON_ZOOM_OUT] = "minus"};
    const int inset = (HUD_BUTTON_SIZE - HUD_BUTTON_ICON) / 2;
    for (int button = HUD_BUTTON_ZOOM_IN; button <= HUD_BUTTON_ZOOM_OUT; ++button) {
        hud_rect_t rect = hud__button_rect(button, width, height);
        display__fill_round_rect(rect.x, rect.y, rect.w, rect.h, 6, HUD_PANEL);
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

    if (state->pin) hud__draw_pin(view, state->pin_lat, state->pin_lon, width, height);
    hud__draw_crosshair(width / 2, height / 2);
    hud__draw_search(view, state, width);
    hud__draw_zoom(view, state->nav_mode, width);
    hud__draw_coordinates(view);
    hud__draw_scale(view, height);
    hud__draw_attribution(width, height);
    if (state->zoom_buttons) hud__draw_buttons(width, height);
    if (state->status != NULL) {
        hud__pill(state->status, state->status_icon, HUD_MARGIN, hud__row_y(2), HUD_ACCENT);
    }
    hud__draw_hint(state->hint, state->hint_icon, width, height);
}
