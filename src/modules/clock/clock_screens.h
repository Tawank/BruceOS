#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock_model.h"
#include "core_sdk/clock.h"
#include "core_sdk/lvgl.h"

typedef enum {
    CLOCK_SCREEN_FACE,
    CLOCK_SCREEN_TIMERS,
    CLOCK_SCREEN_ALARMS,
    CLOCK_SCREEN_STOPWATCH,
    CLOCK_SCREEN_COUNT,
} clock_screen_t;

typedef enum {
    CLOCK_ACTION_NONE,
    CLOCK_ACTION_OPEN_MENU,
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

/* What the widgets were built for; any change means building them again.
 * Compared bytewise, so clear it (memset) before filling it in. */
typedef struct {
    clock_screen_t screen;
    bool menu_open;
    bool preview;
    int16_t width;
    int16_t height;
    bool local_valid;
    bool menu_hint;
    clock_headline_kind_t headline;
    size_t timers;
    size_t alarms;
    size_t laps;
} clock_layout_t;

/* The LVGL widgets of the current screen that change while it shows. */
typedef struct {
    clock_layout_t layout;
    bool built;
    lv_obj_t *big;      /* time, headline or stopwatch */
    lv_obj_t *top;      /* date or caption */
    lv_obj_t *bottom;   /* time of day under a headline */
    lv_obj_t *timer_text[CLOCK_MODEL__MAX_TIMERS];
    lv_obj_t *timer_toggle[CLOCK_MODEL__MAX_TIMERS];
    lv_obj_t *alarm_toggle[CLOCK_MODEL__MAX_ALARMS];
    lv_obj_t *stopwatch_toggle;
    /* Set by a click, run by clock_gui after LVGL returns. */
    clock_action_t action;
    int arg;
} clock_view_t;

void clock_screens__format_seconds(uint32_t seconds, char *out, size_t size);

/* Builds the widgets when `layout` differs from what is shown, then brings
 * every changing text up to date. The face doubles as the process-preview
 * tile: no bar, no hint, just the time or what is running. */
void clock_screens__show(clock_view_t *view, const clock_scene_t *scene, const clock_layout_t *layout);
