#include "clock_overlay.h"

#include "clock_screens.h"
#include "core_sdk/config.h"
#include "core_sdk/icon.h"

#include <string.h>

#define CLOCK_OVERLAY__WIDTH 74
#define CLOCK_OVERLAY__HEIGHT 16
#define CLOCK_OVERLAY__MARGIN 2
#define CLOCK_OVERLAY__TOP_BAR_H 25
#define CLOCK_OVERLAY__PADDING 4
#define CLOCK_OVERLAY__ICON_SIZE 12
#define CLOCK_OVERLAY__CORNER 3
#define CLOCK_OVERLAY__GLYPH_INK_H 11

static bool clock_overlay__create(clock_overlay_t *overlay) {
    int screen_w = display__screen_width();
    int screen_h = display__screen_height();
    if (screen_w < CLOCK_OVERLAY__WIDTH + CLOCK_OVERLAY__MARGIN ||
        screen_h < CLOCK_OVERLAY__TOP_BAR_H + CLOCK_OVERLAY__MARGIN + CLOCK_OVERLAY__HEIGHT) {
        return false;
    }
    int16_t x = (int16_t)(screen_w - CLOCK_OVERLAY__WIDTH - CLOCK_OVERLAY__MARGIN);
    return display__overlay_create(x, CLOCK_OVERLAY__TOP_BAR_H + CLOCK_OVERLAY__MARGIN, CLOCK_OVERLAY__WIDTH, CLOCK_OVERLAY__HEIGHT, &overlay->id) ==
           BRUCE_OK;
}

static bool clock_overlay__draw(const clock_overlay_t *overlay, const clock_headline_t *headline) {
    if (display__overlay_begin(overlay->id) != BRUCE_OK) return false;
    (void)display__fill_round_rect(0, 0, CLOCK_OVERLAY__WIDTH, CLOCK_OVERLAY__HEIGHT, CLOCK_OVERLAY__CORNER, config__get_color_surface());
    (void)display__draw_round_rect(0, 0, CLOCK_OVERLAY__WIDTH, CLOCK_OVERLAY__HEIGHT, CLOCK_OVERLAY__CORNER, config__get_color_primary());

    const bruce_icon_t *icon = icon__get(headline->kind == CLOCK_HEADLINE_STOPWATCH ? "timer-sand" : "timer-outline");
    if (icon != NULL) {
        (void)display__draw_bitmap_scaled(
            CLOCK_OVERLAY__PADDING, (CLOCK_OVERLAY__HEIGHT - CLOCK_OVERLAY__ICON_SIZE) / 2, icon->bits, icon->width, icon->height,
            CLOCK_OVERLAY__ICON_SIZE, CLOCK_OVERLAY__ICON_SIZE, config__get_color_primary()
        );
    }
    char text[16];
    clock_screens__format_seconds(headline->seconds, text, sizeof(text));
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(config__get_color_text());
    (void)display__set_text_size(1);
    (void)display__draw_right_string(text, CLOCK_OVERLAY__WIDTH - CLOCK_OVERLAY__PADDING, (CLOCK_OVERLAY__HEIGHT - CLOCK_OVERLAY__GLYPH_INK_H) / 2);
    return display__overlay_end(overlay->id) == BRUCE_OK;
}

static void clock_overlay__hide(clock_overlay_t *overlay) {
    if (!overlay->visible) return;
    (void)display__overlay_hide(overlay->id);
    overlay->visible = false;
}

void clock_overlay__update(clock_overlay_t *overlay, const clock_headline_t *headline, bool wanted) {
    if (!wanted || headline->kind == CLOCK_HEADLINE_NONE) {
        clock_overlay__hide(overlay);
        return;
    }
    if (overlay->id == BRUCE_DISPLAY_OVERLAY_ID_INVALID && !clock_overlay__create(overlay)) return;
    bool unchanged = overlay->visible && overlay->shown.kind == headline->kind && overlay->shown.seconds == headline->seconds;
    if (unchanged || !clock_overlay__draw(overlay, headline)) return;
    overlay->shown = *headline;
    if (!overlay->visible && display__overlay_show(overlay->id) == BRUCE_OK) overlay->visible = true;
}
