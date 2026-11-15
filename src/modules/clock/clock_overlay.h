#pragma once

#include <stdbool.h>

#include "clock_model.h"
#include "core_sdk/display.h"

typedef struct {
    bruce_display_overlay_id_t id;
    clock_headline_t shown;
    bool visible;
} clock_overlay_t;

/* Keeps a small stopwatch/timer badge in the top-right corner while `wanted` and something is running. */
void clock_overlay__update(clock_overlay_t *overlay, const clock_headline_t *headline, bool wanted);
