#pragma once

#include <stdbool.h>

#include "map_view.h"

/* What Prev/Next do. */
typedef enum {
    HUD_NAV_ZOOM,
    HUD_NAV_UP_DOWN,
    HUD_NAV_LEFT_RIGHT,
    HUD_NAV_COUNT,
} hud_nav_mode_t;

typedef enum {
    HUD_BUTTON_ZOOM_IN,
    HUD_BUTTON_ZOOM_OUT,
} hud_button_t;

typedef struct {
    int x, y, size;
} hud_rect_t;

typedef struct {
    const char *status;
    const char *status_icon;
    hud_nav_mode_t nav_mode;
    const char *hint[2]; /* Short-lived message centred near the bottom. */
    bool zoom_buttons;   /* On-screen +/- for touch screens. */
} hud_state_t;

const char *hud__nav_icon(hud_nav_mode_t mode);
hud_rect_t hud__button_rect(hud_button_t button, int width, int height);
void hud__draw(const map_view_t *view, const hud_state_t *state, int width, int height);
