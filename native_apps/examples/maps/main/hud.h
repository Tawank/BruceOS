#pragma once

#include "map_view.h"

typedef enum {
    HUD_WHEEL_ZOOM,
    HUD_WHEEL_PAN_X,
    HUD_WHEEL_PAN_Y,
} hud_wheel_mode_t;

typedef struct {
    const char *status;
    hud_wheel_mode_t wheel_mode;
} hud_state_t;

void hud__draw(const map_view_t *view, const hud_state_t *state, int width, int height);
