#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock_model.h"
#include "clock_ui.h"
#include "core_sdk/clock.h"

typedef enum {
    CLOCK_SCREEN_FACE,
    CLOCK_SCREEN_TIMERS,
    CLOCK_SCREEN_ALARMS,
    CLOCK_SCREEN_STOPWATCH,
    CLOCK_SCREEN_COUNT,
} clock_screen_t;

typedef enum {
    CLOCK_ACTION_GO_SCREEN,
    CLOCK_ACTION_CLOSE,
    CLOCK_ACTION_ADD_TIMER,
    CLOCK_ACTION_CUSTOM_TIMER,
    CLOCK_ACTION_TOGGLE_TIMER,
    CLOCK_ACTION_REMOVE_TIMER,
    CLOCK_ACTION_ADD_ALARM,
    CLOCK_ACTION_TOGGLE_ALARM,
    CLOCK_ACTION_REMOVE_ALARM,
    CLOCK_ACTION_STOPWATCH_TOGGLE,
    CLOCK_ACTION_STOPWATCH_LAP,
    CLOCK_ACTION_STOPWATCH_RESET,
} clock_action_t;

typedef struct {
    const clock_model_t *model;
    const bruce_clock_datetime_t *local; /* NULL while the clock is unset */
    uint64_t now_ms;
    int16_t width;
    int16_t height;
    bool show_menu_hint;
} clock_scene_t;

void clock_screens__format_seconds(uint32_t seconds, char *out, size_t size);

/* The face doubles as the process-preview tile: no bar, no hint, just the time. */
void clock_screens__build_preview(clock_ui_t *ui, const clock_scene_t *scene);
void clock_screens__build(clock_ui_t *ui, const clock_scene_t *scene, clock_screen_t screen, bool menu_open);
