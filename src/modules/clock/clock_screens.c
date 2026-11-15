#include "clock_screens.h"

#include "core_sdk/config.h"

#include <stdio.h>

#define CLOCK_SCREENS__GAP 3
#define CLOCK_SCREENS__GLYPH_H 10
#define CLOCK_SCREENS__TALL_DISPLAY_H 100
#define CLOCK_SCREENS__PREVIEW_DETAIL_H 60

typedef struct {
    int16_t margin;
    int16_t inner_w;
    int16_t button_h;
    int16_t row_h;
    int16_t bar_h;
    int16_t content_h;
    bool bar_labels;
} clock_layout_t;

static clock_layout_t clock_screens__layout(const clock_scene_t *scene) {
    bool tall = scene->height >= CLOCK_SCREENS__TALL_DISPLAY_H;
    clock_layout_t layout = {
        .margin = tall ? 6 : 3,
        .button_h = tall ? 22 : 16,
        .row_h = tall ? 24 : 18,
        .bar_h = tall ? 34 : 22,
        .bar_labels = tall,
    };
    layout.inner_w = scene->width - 2 * layout.margin;
    layout.content_h = scene->height - layout.bar_h;
    return layout;
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
        snprintf(out, size, "%u:%02u:%02u.%02u", (unsigned)(seconds / 3600u), (unsigned)(seconds / 60u % 60u), (unsigned)(seconds % 60u), centis);
    } else {
        snprintf(out, size, "%02u:%02u.%02u", (unsigned)(seconds / 60u), (unsigned)(seconds % 60u), centis);
    }
}

static void clock_screens__add_title(clock_ui_t *ui, const clock_layout_t *layout, int16_t *y, const char *title) {
    clock_ui__label(ui, layout->margin, *y, layout->inner_w, CLOCK_SCREENS__GLYPH_H + 2, title)->tone = CLOCK_TONE_MUTED;
    *y += CLOCK_SCREENS__GLYPH_H + 2 + CLOCK_SCREENS__GAP;
}

/* Splits the content width into `count` equal buttons on one row. */
static int16_t clock_screens__cell_w(const clock_layout_t *layout, int count) {
    return (layout->inner_w - (count - 1) * CLOCK_SCREENS__GAP) / count;
}

static int16_t clock_screens__cell_x(const clock_layout_t *layout, int index) {
    return layout->margin + index * (clock_screens__cell_w(layout, 5) + CLOCK_SCREENS__GAP);
}

static void clock_screens__add_bar(clock_ui_t *ui, const clock_scene_t *scene, const clock_layout_t *layout, clock_screen_t current) {
    static const struct {
        const char *icon;
        const char *label;
    } entries[] = {
        {"clock-outline", "Clock"},
        {"timer-outline", "Timers"},
        {"alarm", "Alarms"},
        {"timer-sand", "Watch"},
        {"close", "Close"},
    };
    for (int i = 0; i < CLOCK_SCREEN_COUNT + 1; ++i) {
        bool is_close = i == CLOCK_SCREEN_COUNT;
        clock_item_t *item = clock_ui__button(
            ui,
            clock_screens__cell_x(layout, i),
            scene->height - layout->bar_h,
            clock_screens__cell_w(layout, 5),
            layout->bar_h - CLOCK_SCREENS__GAP,
            layout->bar_labels ? entries[i].label : "",
            entries[i].icon,
            is_close ? CLOCK_ACTION_CLOSE : CLOCK_ACTION_GO_SCREEN,
            i
        );
        if (item != NULL) item->current = !is_close && i == (int)current;
    }
}

static void clock_screens__add_face(clock_ui_t *ui, const clock_scene_t *scene, int16_t viewport_h, bool with_date) {
    int16_t width = scene->width;
    if (scene->local == NULL) {
        clock_ui__label(ui, 0, 0, width, viewport_h, "--:--:--")->text_size = clock_ui__fit_text_size("--:--:--", width, viewport_h, 4);
        return;
    }
    char time[16];
    char date[16];
    clock_screens__format_time_of_day(scene->local, time, sizeof(time));
    int16_t date_h = with_date ? CLOCK_SCREENS__GLYPH_H + 6 : 0;
    clock_item_t *big = clock_ui__label(ui, 0, date_h, width, viewport_h - 2 * date_h, time);
    big->text_size = clock_ui__fit_text_size(time, width - 8, viewport_h - 2 * date_h, 4);
    big->tone = CLOCK_TONE_PRIMARY;
    if (!with_date) return;
    snprintf(date, sizeof(date), "%04u-%02u-%02u", scene->local->year, scene->local->month, scene->local->day);
    clock_ui__label(ui, 0, 0, width, date_h, date)->tone = CLOCK_TONE_MUTED;
}

static void clock_screens__add_headline(clock_ui_t *ui, const clock_scene_t *scene, const clock_headline_t *headline) {
    bool detailed = scene->height >= CLOCK_SCREENS__PREVIEW_DETAIL_H;
    int16_t band_h = detailed ? CLOCK_SCREENS__GLYPH_H + 6 : 0;
    int16_t big_h = scene->height - 2 * band_h;
    char text[CLOCK_UI__LABEL_MAX];
    clock_screens__format_seconds(headline->seconds, text, sizeof(text));
    clock_item_t *big = clock_ui__label(ui, 0, band_h, scene->width, big_h, text);
    big->text_size = clock_ui__fit_text_size(text, scene->width - 8, big_h, 4);
    big->tone = CLOCK_TONE_PRIMARY;
    if (!detailed) return;

    const char *caption = headline->kind == CLOCK_HEADLINE_STOPWATCH ? "Stopwatch" : "Timer";
    clock_ui__label(ui, 0, 0, scene->width, band_h, caption)->tone = CLOCK_TONE_MUTED;
    if (scene->local == NULL) return;
    char now[CLOCK_UI__LABEL_MAX];
    clock_screens__format_time_of_day(scene->local, now, sizeof(now));
    clock_ui__label(ui, 0, scene->height - band_h, scene->width, band_h, now)->tone = CLOCK_TONE_MUTED;
}

/* Previewed (or tiled) clocks show what matters most: stopwatch, then the soonest timer, then the time. */
void clock_screens__build_preview(clock_ui_t *ui, const clock_scene_t *scene) {
    clock_ui__begin(ui, scene->height);
    clock_headline_t headline = clock_model__headline(scene->model, scene->now_ms);
    if (headline.kind != CLOCK_HEADLINE_NONE) {
        clock_screens__add_headline(ui, scene, &headline);
        return;
    }
    clock_screens__add_face(ui, scene, scene->height, scene->height >= CLOCK_SCREENS__PREVIEW_DETAIL_H);
}

static void clock_screens__build_face(clock_ui_t *ui, const clock_scene_t *scene, const clock_layout_t *layout, bool menu_open) {
    int16_t viewport_h = menu_open ? layout->content_h : scene->height;
    clock_ui__begin(ui, viewport_h);
    clock_screens__add_face(ui, scene, viewport_h, true);
    if (menu_open) clock_screens__add_bar(ui, scene, layout, CLOCK_SCREEN_FACE);
    else if (scene->show_menu_hint) {
        clock_ui__label(ui, 0, scene->height - CLOCK_SCREENS__GLYPH_H - 4, scene->width, CLOCK_SCREENS__GLYPH_H, "OK for menu")->tone = CLOCK_TONE_MUTED;
    }
}

/* One list row: a label that takes the leftover width, then two square icon buttons. */
static void clock_screens__add_row(
    clock_ui_t *ui, const clock_layout_t *layout, int16_t y, const char *text, bool dimmed, const char *toggle_icon, int toggle_action,
    int remove_action, int index
) {
    int16_t button = layout->row_h;
    int16_t text_w = layout->inner_w - 2 * (button + CLOCK_SCREENS__GAP);
    clock_item_t *label = clock_ui__label(ui, layout->margin, y, text_w, button, text);
    label->text_size = clock_ui__fit_text_size(text, text_w, button, 2);
    label->tone = dimmed ? CLOCK_TONE_MUTED : CLOCK_TONE_TEXT;
    int16_t toggle_x = layout->margin + text_w + CLOCK_SCREENS__GAP;
    clock_ui__button(ui, toggle_x, y, button, button, "", toggle_icon, toggle_action, index);
    clock_ui__button(ui, toggle_x + button + CLOCK_SCREENS__GAP, y, button, button, "", "close", remove_action, index);
}

static void clock_screens__build_timers(clock_ui_t *ui, const clock_scene_t *scene, const clock_layout_t *layout) {
    static const struct {
        const char *label;
        uint32_t seconds;
    } presets[] = {{"1m", 60}, {"5m", 300}, {"10m", 600}, {"15m", 900}};
    const int preset_count = sizeof(presets) / sizeof(presets[0]);
    clock_ui__begin(ui, layout->content_h);

    int16_t y = layout->margin;
    clock_screens__add_title(ui, layout, &y, "Add timer");
    for (int i = 0; i < preset_count; ++i) {
        int16_t w = clock_screens__cell_w(layout, preset_count);
        clock_ui__button(ui, layout->margin + i * (w + CLOCK_SCREENS__GAP), y, w, layout->button_h, presets[i].label, NULL, CLOCK_ACTION_ADD_TIMER, (int)presets[i].seconds);
    }
    y += layout->button_h + CLOCK_SCREENS__GAP;
    clock_ui__button(ui, layout->margin, y, layout->inner_w, layout->button_h, "Custom", NULL, CLOCK_ACTION_CUSTOM_TIMER, 0);
    y += layout->button_h + CLOCK_SCREENS__GAP * 2;

    if (scene->model->timer_count == 0) clock_screens__add_title(ui, layout, &y, "No timers");
    for (size_t i = 0; i < scene->model->timer_count; ++i) {
        const clock_timer_t *timer = &scene->model->timers[i];
        char text[CLOCK_UI__LABEL_MAX];
        clock_screens__format_seconds(clock_timer__remaining_s(timer, scene->now_ms), text, sizeof(text));
        clock_screens__add_row(ui, layout, y, text, !timer->running, timer->running ? "pause" : "play", CLOCK_ACTION_TOGGLE_TIMER, CLOCK_ACTION_REMOVE_TIMER, (int)i);
        y += layout->row_h + CLOCK_SCREENS__GAP;
    }
    clock_screens__add_bar(ui, scene, layout, CLOCK_SCREEN_TIMERS);
}

static void clock_screens__build_alarms(clock_ui_t *ui, const clock_scene_t *scene, const clock_layout_t *layout) {
    clock_ui__begin(ui, layout->content_h);

    int16_t y = layout->margin;
    clock_ui__button(ui, layout->margin, y, layout->inner_w, layout->button_h, "+ Add alarm", NULL, CLOCK_ACTION_ADD_ALARM, 0);
    y += layout->button_h + CLOCK_SCREENS__GAP * 2;

    if (scene->model->alarm_count == 0) clock_screens__add_title(ui, layout, &y, "No alarms");
    for (size_t i = 0; i < scene->model->alarm_count; ++i) {
        const clock_alarm_t *alarm = &scene->model->alarms[i];
        char text[CLOCK_UI__LABEL_MAX];
        snprintf(text, sizeof(text), "%02u:%02u", (unsigned)(alarm->seconds_of_day / 3600u), (unsigned)(alarm->seconds_of_day / 60u % 60u));
        clock_screens__add_row(ui, layout, y, text, !alarm->enabled, alarm->enabled ? "toggle-switch" : "toggle-switch-off", CLOCK_ACTION_TOGGLE_ALARM, CLOCK_ACTION_REMOVE_ALARM, (int)i);
        y += layout->row_h + CLOCK_SCREENS__GAP;
    }
    clock_screens__add_bar(ui, scene, layout, CLOCK_SCREEN_ALARMS);
}

static void clock_screens__build_stopwatch(clock_ui_t *ui, const clock_scene_t *scene, const clock_layout_t *layout) {
    const clock_stopwatch_t *stopwatch = &scene->model->stopwatch;
    clock_ui__begin(ui, layout->content_h);

    char text[CLOCK_UI__LABEL_MAX];
    clock_screens__format_centiseconds(clock_stopwatch__elapsed_ms(stopwatch, scene->now_ms), text, sizeof(text));
    int16_t y = layout->margin;
    int16_t display_h = layout->row_h + CLOCK_SCREENS__GLYPH_H;
    clock_item_t *elapsed = clock_ui__label(ui, layout->margin, y, layout->inner_w, display_h, text);
    elapsed->text_size = clock_ui__fit_text_size(text, layout->inner_w, display_h, 3);
    elapsed->tone = CLOCK_TONE_PRIMARY;
    y += display_h + CLOCK_SCREENS__GAP;

    int16_t w = clock_screens__cell_w(layout, 3);
    static const char *const stopped_labels[] = {"Start", "Lap", "Reset"};
    for (int i = 0; i < 3; ++i) {
        const char *label = i == 0 && stopwatch->running ? "Stop" : stopped_labels[i];
        int action = i == 0 ? CLOCK_ACTION_STOPWATCH_TOGGLE : i == 1 ? CLOCK_ACTION_STOPWATCH_LAP : CLOCK_ACTION_STOPWATCH_RESET;
        clock_ui__button(ui, layout->margin + i * (w + CLOCK_SCREENS__GAP), y, w, layout->button_h, label, NULL, action, 0);
    }
    y += layout->button_h + CLOCK_SCREENS__GAP;

    for (size_t i = stopwatch->lap_count; i > 0; --i) {
        char lap[CLOCK_UI__LABEL_MAX];
        int prefix = snprintf(lap, sizeof(lap), "%u ", (unsigned)(i % 100));
        clock_screens__format_centiseconds(stopwatch->laps_ms[i - 1], lap + prefix, sizeof(lap) - (size_t)prefix);
        clock_ui__label(ui, layout->margin, y, layout->inner_w, CLOCK_SCREENS__GLYPH_H + 2, lap)->tone = CLOCK_TONE_MUTED;
        y += CLOCK_SCREENS__GLYPH_H + 2;
    }
    clock_screens__add_bar(ui, scene, layout, CLOCK_SCREEN_STOPWATCH);
}

void clock_screens__build(clock_ui_t *ui, const clock_scene_t *scene, clock_screen_t screen, bool menu_open) {
    clock_layout_t layout = clock_screens__layout(scene);
    switch (screen) {
    case CLOCK_SCREEN_TIMERS: clock_screens__build_timers(ui, scene, &layout); break;
    case CLOCK_SCREEN_ALARMS: clock_screens__build_alarms(ui, scene, &layout); break;
    case CLOCK_SCREEN_STOPWATCH: clock_screens__build_stopwatch(ui, scene, &layout); break;
    default: clock_screens__build_face(ui, scene, &layout, menu_open); break;
    }
}
