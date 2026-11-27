#include "clock_screens.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/config.h"
#include "core_sdk/input.h"

#define CLOCK_SCREENS__GAP 3
#define CLOCK_SCREENS__TALL_DISPLAY_H 100
#define CLOCK_SCREENS__PREVIEW_DETAIL_H 60
#define CLOCK_SCREENS__BIG_SCALE_MAX 4
#define CLOCK_SCREENS__ICON_MAX 20

/* Sizes follow the screen height, as the clock has always done. */
typedef struct {
    int16_t margin;
    int16_t button_h;
    int16_t row_h;
    int16_t bar_h;
    bool bar_labels;
} clock_sizes_t;

static clock_sizes_t clock_screens__sizes(int16_t height) {
    bool tall = height >= CLOCK_SCREENS__TALL_DISPLAY_H;
    return (clock_sizes_t){
        .margin = tall ? 6 : 3,
        .button_h = tall ? 22 : 16,
        .row_h = tall ? 24 : 18,
        .bar_h = tall ? 34 : 22,
        .bar_labels = tall,
    };
}

void clock_screens__format_seconds(uint32_t seconds, char *out, size_t size) {
    snprintf(out, size, "%02u:%02u:%02u", (unsigned)(seconds / 3600u), (unsigned)(seconds / 60u % 60u), (unsigned)(seconds % 60u));
}

static void clock_screens__format_time_of_day(const bruce_clock_datetime_t *local, char *out, size_t size) {
    if (config__get_time_clock24hr()) {
        snprintf(out, size, "%02u:%02u:%02u", local->hour, local->minute, local->second);
        return;
    }
    unsigned hour = local->hour % 12u == 0 ? 12u : local->hour % 12u;
    snprintf(out, size, "%02u:%02u:%02u %s", hour, local->minute, local->second, local->hour < 12 ? "AM" : "PM");
}

static void clock_screens__format_centiseconds(uint32_t milliseconds, char *out, size_t size) {
    uint32_t seconds = milliseconds / 1000u;
    unsigned centis = milliseconds / 10u % 100u;
    if (seconds >= 3600u) {
        snprintf(out, size, "%u:%02u:%02u.%02u", (unsigned)(seconds / 3600u), (unsigned)(seconds / 60u % 60u),
                 (unsigned)(seconds % 60u), centis);
    } else {
        snprintf(out, size, "%02u:%02u.%02u", (unsigned)(seconds / 60u), (unsigned)(seconds % 60u), centis);
    }
}

/* Only a real change redraws, which matters on e-paper. */
static void clock_screens__set_text(lv_obj_t *label, const char *text) {
    if (label != NULL && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static lv_obj_t *clock_screens__label(lv_obj_t *parent, const char *text, bool muted) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (muted) lv_obj_set_style_text_color(label, lvgl__color(config__get_color_text_muted()), 0);
    return label;
}

/* Big monospace digits in the primary color: Bruce's font at the largest
 * scale where `widest` fits w x h, so the digits never move as they change. */
static lv_obj_t *clock_screens__big(lv_obj_t *parent, const char *widest, int16_t w, int16_t h) {
    const lv_font_t *font = lvgl__font(1);
    for (int scale = CLOCK_SCREENS__BIG_SCALE_MAX; scale > 1; --scale) {
        const lv_font_t *candidate = lvgl__font(scale);
        if (lv_text_get_width(widest, (uint32_t)strlen(widest), candidate, 0) <= w &&
            lv_font_get_line_height(candidate) <= h) {
            font = candidate;
            break;
        }
    }
    lv_obj_t *label = clock_screens__label(parent, "", false);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lvgl__color(config__get_color_primary()), 0);
    return label;
}

/* A bare flex container: no panel look, just layout. */
static lv_obj_t *clock_screens__box(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_style_pad_gap(box, CLOCK_SCREENS__GAP, 0);
    return box;
}

static void clock_screens__clicked(lv_event_t *event) {
    clock_view_t *view = lv_event_get_user_data(event);
    int32_t packed = (int32_t)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    view->action = (clock_action_t)(packed >> 16);
    view->arg = packed & 0xFFFF;
}

static void clock_screens__on_click(clock_view_t *view, lv_obj_t *obj, clock_action_t action, int arg) {
    lv_obj_set_user_data(obj, (void *)(intptr_t)(((int32_t)action << 16) | (arg & 0xFFFF)));
    lv_obj_add_event_cb(obj, clock_screens__clicked, LV_EVENT_CLICKED, view);
}

/* A button with an icon, a label, or both stacked (icon above). */
static lv_obj_t *clock_screens__button(
    clock_view_t *view, lv_obj_t *parent, const char *text, const char *icon, int16_t h, clock_action_t action, int arg
) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, h);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(button, 1, 0);
    bool stacked = icon != NULL && text != NULL;
    if (icon != NULL) {
        int32_t size = h - 4 - (stacked ? lv_font_get_line_height(lvgl__font(1)) : 0);
        if (size > CLOCK_SCREENS__ICON_MAX) size = CLOCK_SCREENS__ICON_MAX;
        lvgl__icon_create(button, icon, size);
    }
    if (text != NULL) lv_label_set_text(lv_label_create(button), text);
    clock_screens__on_click(view, button, action, arg);
    return button;
}

/* A square icon button whose icon swaps between two (pause/play, on/off). */
static lv_obj_t *clock_screens__toggle(
    clock_view_t *view, lv_obj_t *parent, const char *on, const char *off, int16_t size, clock_action_t action, int arg
) {
    lv_obj_t *button = clock_screens__button(view, parent, NULL, on, size, action, arg);
    lv_obj_set_width(button, size);
    int32_t icon = size - 4 > CLOCK_SCREENS__ICON_MAX ? CLOCK_SCREENS__ICON_MAX : size - 4;
    lv_obj_t *second = lvgl__icon_create(button, off, icon);
    if (second != NULL) lv_obj_add_flag(second, LV_OBJ_FLAG_HIDDEN);
    return button;
}

/* Shows the first icon of a toggle when `on`, else the second. */
static void clock_screens__set_toggle(lv_obj_t *toggle, bool on) {
    lv_obj_t *first = toggle != NULL ? lv_obj_get_child(toggle, 0) : NULL;
    lv_obj_t *second = toggle != NULL ? lv_obj_get_child(toggle, 1) : NULL;
    if (first == NULL || second == NULL || lv_obj_has_flag(first, LV_OBJ_FLAG_HIDDEN) == !on) return;
    if (on) {
        lv_obj_remove_flag(first, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(second, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(first, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(second, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- Pieces ----------------------------------------------------------------- */

static void clock_screens__add_bar(clock_view_t *view, lv_obj_t *screen, const clock_sizes_t *sizes, clock_screen_t current) {
    static const struct {
        const char *icon;
        const char *label;
    } entries[] = {
        {"clock-outline", "Clock"}, {"timer-outline", "Timers"}, {"alarm", "Alarms"}, {"timer-sand", "Watch"}, {"close", "Close"},
    };
    lv_obj_t *bar = clock_screens__box(screen, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(bar, LV_PCT(100), sizes->bar_h);
    lv_obj_set_style_pad_hor(bar, sizes->margin, 0);
    lv_obj_set_style_pad_bottom(bar, CLOCK_SCREENS__GAP, 0);
    for (int i = 0; i < CLOCK_SCREEN_COUNT + 1; ++i) {
        bool close = i == CLOCK_SCREEN_COUNT;
        lv_obj_t *button = clock_screens__button(
            view, bar, sizes->bar_labels ? entries[i].label : NULL, entries[i].icon, sizes->bar_h - CLOCK_SCREENS__GAP,
            close ? CLOCK_ACTION_CLOSE : CLOCK_ACTION_GO_SCREEN, i
        );
        lv_obj_set_flex_grow(button, 1);
        if (!close && i == (int)current) lv_obj_add_state(button, LV_STATE_CHECKED);
    }
}

/* The area above the bar: fills the rest of the screen and scrolls. */
static lv_obj_t *clock_screens__content(lv_obj_t *screen, const clock_sizes_t *sizes, bool centered) {
    lv_obj_t *content = clock_screens__box(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, sizes->margin, 0);
    if (centered) lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return content;
}

static int16_t clock_screens__line_h(void) { return (int16_t)lv_font_get_line_height(lvgl__font(1)); }

static void clock_screens__build_face(clock_view_t *view, lv_obj_t *content, const clock_layout_t *layout,
                                      const clock_sizes_t *sizes, int16_t height, bool details) {
    if (details) view->top = clock_screens__label(content, "", true);
    const char *widest = config__get_time_clock24hr() ? "00:00:00" : "00:00:00 AM";
    view->big = clock_screens__big(content, widest, (int16_t)(layout->width - 2 * sizes->margin),
                                   (int16_t)(height - 2 * sizes->margin - (details ? 3 * clock_screens__line_h() : 0)));
    if (layout->menu_hint) clock_screens__label(content, input__has_touch() ? "Tap for menu" : "OK for menu", true);
}

static void clock_screens__build_headline(clock_view_t *view, lv_obj_t *content, const clock_layout_t *layout,
                                          const clock_sizes_t *sizes) {
    bool details = layout->height >= CLOCK_SCREENS__PREVIEW_DETAIL_H;
    if (details) {
        clock_screens__label(content, layout->headline == CLOCK_HEADLINE_STOPWATCH ? "Stopwatch" : "Timer", true);
    }
    view->big = clock_screens__big(content, "00:00:00", (int16_t)(layout->width - 2 * sizes->margin),
                                   (int16_t)(layout->height - 2 * sizes->margin - (details ? 2 * clock_screens__line_h() : 0)));
    if (details) view->bottom = clock_screens__label(content, "", true);
}

/* One list row: the text taking the leftover width, then a toggle and a remove button. */
static lv_obj_t *clock_screens__row(lv_obj_t *content, const clock_sizes_t *sizes, lv_obj_t **out_text) {
    lv_obj_t *row = clock_screens__box(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, LV_PCT(100), sizes->row_h);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    *out_text = clock_screens__label(row, "", false);
    lv_obj_set_flex_grow(*out_text, 1);
    return row;
}

static void clock_screens__add_remove(clock_view_t *view, lv_obj_t *line, const clock_sizes_t *sizes,
                                      clock_action_t action, size_t index) {
    lv_obj_t *button = clock_screens__button(view, line, NULL, "close", sizes->row_h, action, (int)index);
    lv_obj_set_width(button, sizes->row_h);
}

static void clock_screens__build_timers(clock_view_t *view, lv_obj_t *content, const clock_scene_t *scene,
                                        const clock_sizes_t *sizes) {
    static const struct {
        const char *label;
        int seconds;
    } presets[] = {{"1m", 60}, {"5m", 300}, {"10m", 600}, {"15m", 900}};
    clock_screens__label(content, "Add timer", true);
    lv_obj_t *row = clock_screens__box(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i) {
        lv_obj_t *button = clock_screens__button(view, row, presets[i].label, NULL, sizes->button_h,
                                                 CLOCK_ACTION_ADD_TIMER, presets[i].seconds);
        lv_obj_set_flex_grow(button, 1);
    }
    lv_obj_t *custom = clock_screens__button(view, content, "Custom", NULL, sizes->button_h, CLOCK_ACTION_CUSTOM_TIMER, 0);
    lv_obj_set_width(custom, LV_PCT(100));
    if (scene->model->timer_count == 0) clock_screens__label(content, "No timers", true);
    for (size_t i = 0; i < scene->model->timer_count; ++i) {
        lv_obj_t *line = clock_screens__row(content, sizes, &view->timer_text[i]);
        view->timer_toggle[i] =
            clock_screens__toggle(view, line, "pause", "play", sizes->row_h, CLOCK_ACTION_TOGGLE_TIMER, (int)i);
        clock_screens__add_remove(view, line, sizes, CLOCK_ACTION_REMOVE_TIMER, i);
    }
}

static void clock_screens__build_alarms(clock_view_t *view, lv_obj_t *content, const clock_scene_t *scene,
                                        const clock_sizes_t *sizes) {
    lv_obj_t *add = clock_screens__button(view, content, "+ Add alarm", NULL, sizes->button_h, CLOCK_ACTION_ADD_ALARM, 0);
    lv_obj_set_width(add, LV_PCT(100));
    if (scene->model->alarm_count == 0) clock_screens__label(content, "No alarms", true);
    for (size_t i = 0; i < scene->model->alarm_count; ++i) {
        const clock_alarm_t *alarm = &scene->model->alarms[i];
        lv_obj_t *text;
        lv_obj_t *line = clock_screens__row(content, sizes, &text);
        char time[16];
        snprintf(time, sizeof(time), "%02u:%02u", (unsigned)(alarm->seconds_of_day / 3600u % 24u),
                 (unsigned)(alarm->seconds_of_day / 60u % 60u));
        lv_label_set_text(text, time);
        view->alarm_toggle[i] = clock_screens__toggle(view, line, "toggle-switch", "toggle-switch-off", sizes->row_h,
                                                      CLOCK_ACTION_TOGGLE_ALARM, (int)i);
        clock_screens__add_remove(view, line, sizes, CLOCK_ACTION_REMOVE_ALARM, i);
    }
}

static void clock_screens__build_stopwatch(clock_view_t *view, lv_obj_t *content, const clock_scene_t *scene,
                                           const clock_layout_t *layout, const clock_sizes_t *sizes) {
    view->big = clock_screens__big(content, "00:00.00", (int16_t)(layout->width - 2 * sizes->margin),
                                   (int16_t)(sizes->row_h + 2 * clock_screens__line_h()));
    lv_obj_t *row = clock_screens__box(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    static const struct {
        const char *label;
        clock_action_t action;
    } buttons[] = {
        {"Start", CLOCK_ACTION_STOPWATCH_TOGGLE}, {"Lap", CLOCK_ACTION_STOPWATCH_LAP}, {"Reset", CLOCK_ACTION_STOPWATCH_RESET},
    };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        lv_obj_t *button = clock_screens__button(view, row, buttons[i].label, NULL, sizes->button_h, buttons[i].action, 0);
        lv_obj_set_flex_grow(button, 1);
        if (i == 0) view->stopwatch_toggle = button;
    }
    const clock_stopwatch_t *stopwatch = &scene->model->stopwatch;
    for (size_t i = stopwatch->lap_count; i > 0; --i) {
        char lap[24];
        int prefix = snprintf(lap, sizeof(lap), "%u ", (unsigned)(i % 100));
        clock_screens__format_centiseconds(stopwatch->laps_ms[i - 1], lap + prefix, sizeof(lap) - (size_t)prefix);
        clock_screens__label(content, lap, true);
    }
}

static void clock_screens__build(clock_view_t *view, const clock_scene_t *scene, const clock_layout_t *layout) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    memset(view, 0, sizeof(*view));
    view->layout = *layout;
    view->built = true;
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_gap(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_CLICKABLE);

    clock_sizes_t sizes = clock_screens__sizes(layout->height);
    bool face = layout->preview || layout->screen == CLOCK_SCREEN_FACE;
    bool bar = !layout->preview && (layout->menu_open || layout->screen != CLOCK_SCREEN_FACE);
    int16_t content_h = (int16_t)(layout->height - (bar ? sizes.bar_h : 0));
    lv_obj_t *content = clock_screens__content(screen, &sizes, face || layout->screen == CLOCK_SCREEN_STOPWATCH);
    if (layout->preview && layout->headline != CLOCK_HEADLINE_NONE) {
        clock_screens__build_headline(view, content, layout, &sizes);
    } else if (face) {
        clock_screens__build_face(view, content, layout, &sizes, content_h,
                                  !layout->preview || layout->height >= CLOCK_SCREENS__PREVIEW_DETAIL_H);
    } else if (layout->screen == CLOCK_SCREEN_TIMERS) {
        clock_screens__build_timers(view, content, scene, &sizes);
    } else if (layout->screen == CLOCK_SCREEN_ALARMS) {
        clock_screens__build_alarms(view, content, scene, &sizes);
    } else {
        clock_screens__build_stopwatch(view, content, scene, layout, &sizes);
    }
    /* The bare face opens the menu on a tap anywhere; its content fills the screen. */
    if (face && !bar && !layout->preview) clock_screens__on_click(view, content, CLOCK_ACTION_OPEN_MENU, 0);
    if (bar) clock_screens__add_bar(view, screen, &sizes, layout->screen);
}

/* ---- Updating ------------------------------------------------------------------ */

static void clock_screens__update(clock_view_t *view, const clock_scene_t *scene) {
    const clock_layout_t *layout = &view->layout;
    char text[24];
    if (layout->preview && layout->headline != CLOCK_HEADLINE_NONE) {
        clock_headline_t headline = clock_model__headline(scene->model, scene->now_ms);
        clock_screens__format_seconds(headline.seconds, text, sizeof(text));
        clock_screens__set_text(view->big, text);
        if (scene->local != NULL) clock_screens__format_time_of_day(scene->local, text, sizeof(text));
        else snprintf(text, sizeof(text), "--:--:--");
        clock_screens__set_text(view->bottom, text);
        return;
    }
    switch (layout->preview ? CLOCK_SCREEN_FACE : layout->screen) {
        case CLOCK_SCREEN_FACE:
            if (scene->local == NULL) {
                clock_screens__set_text(view->big, "--:--:--");
                clock_screens__set_text(view->top, "");
                break;
            }
            clock_screens__format_time_of_day(scene->local, text, sizeof(text));
            clock_screens__set_text(view->big, text);
            snprintf(text, sizeof(text), "%04u-%02u-%02u", scene->local->year, scene->local->month, scene->local->day);
            clock_screens__set_text(view->top, text);
            break;
        case CLOCK_SCREEN_TIMERS:
            for (size_t i = 0; i < scene->model->timer_count; ++i) {
                const clock_timer_t *timer = &scene->model->timers[i];
                clock_screens__format_seconds(clock_timer__remaining_s(timer, scene->now_ms), text, sizeof(text));
                clock_screens__set_text(view->timer_text[i], text);
                uint16_t color = timer->running ? config__get_color_text() : config__get_color_text_muted();
                lv_obj_set_style_text_color(view->timer_text[i], lvgl__color(color), 0);
                clock_screens__set_toggle(view->timer_toggle[i], timer->running);
            }
            break;
        case CLOCK_SCREEN_ALARMS:
            for (size_t i = 0; i < scene->model->alarm_count; ++i) {
                clock_screens__set_toggle(view->alarm_toggle[i], scene->model->alarms[i].enabled);
            }
            break;
        case CLOCK_SCREEN_STOPWATCH: {
            const clock_stopwatch_t *stopwatch = &scene->model->stopwatch;
            clock_screens__format_centiseconds(clock_stopwatch__elapsed_ms(stopwatch, scene->now_ms), text, sizeof(text));
            clock_screens__set_text(view->big, text);
            clock_screens__set_text(lv_obj_get_child(view->stopwatch_toggle, 0), stopwatch->running ? "Stop" : "Start");
            break;
        }
        default: break;
    }
}

void clock_screens__show(clock_view_t *view, const clock_scene_t *scene, const clock_layout_t *layout) {
    if (!view->built || memcmp(&view->layout, layout, sizeof(*layout)) != 0) clock_screens__build(view, scene, layout);
    clock_screens__update(view, scene);
}
