#include "clock_gui.h"

#include "clock_overlay.h"
#include "core_sdk/app_config.h"
#include "core_sdk/audio.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/notification.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"

#include <stdio.h>

#define CLOCK_GUI__MENU_HINT_MS 5000u
#define CLOCK_GUI__IDLE_POLL_MS 100u
#define CLOCK_GUI__STOPWATCH_POLL_MS 40u
#define CLOCK_GUI__NOTIFICATION_MS 10000u
#define CLOCK_GUI__ALARMS_KEY "alarms"
#define CLOCK_GUI__ALARM_ENABLED_BIT (1 << 17)

typedef struct {
    clock_model_t model;
    clock_ui_t ui;
    clock_overlay_t overlay;
    clock_screen_t screen;
    bool menu_open;
    uint64_t started_ms;
    uint32_t drawn_signature;
    bool has_drawn;
    bool backgrounded;
} clock_gui_t;

typedef struct {
    bool valid;
    bruce_clock_datetime_t local;
    int64_t seconds;
} clock_now_t;

static clock_now_t clock_gui__now(void) {
    clock_now_t now = {0};
    now.valid = clock__get_local(&now.local) == BRUCE_OK && clock__datetime_to_epoch(&now.local, &now.seconds) == BRUCE_OK;
    return now;
}

static void clock_gui__save_alarms(const clock_model_t *model) {
    int values[CLOCK_MODEL__MAX_ALARMS];
    for (size_t i = 0; i < model->alarm_count; ++i) {
        values[i] = (int)model->alarms[i].seconds_of_day | (model->alarms[i].enabled ? CLOCK_GUI__ALARM_ENABLED_BIT : 0);
    }
    (void)app_config__set_int_array(NULL, CLOCK_GUI__ALARMS_KEY, values, model->alarm_count);
}

static void clock_gui__load_alarms(clock_model_t *model, const clock_now_t *now) {
    int values[CLOCK_MODEL__MAX_ALARMS];
    size_t count = app_config__get_int_array(NULL, CLOCK_GUI__ALARMS_KEY, values, CLOCK_MODEL__MAX_ALARMS);
    for (size_t i = 0; i < count && now->valid; ++i) {
        uint32_t seconds_of_day = (uint32_t)values[i] & (CLOCK_GUI__ALARM_ENABLED_BIT - 1);
        if (!clock_model__add_alarm(model, seconds_of_day, now->seconds)) break;
        if (!(values[i] & CLOCK_GUI__ALARM_ENABLED_BIT)) clock_model__toggle_alarm(model, model->alarm_count - 1, now->seconds);
    }
}

static void clock_gui__announce(const clock_event_t *event) {
    char text[40];
    char formatted[16];
    clock_screens__format_seconds(event->seconds, formatted, sizeof(formatted));
    if (event->kind == CLOCK_EVENT_TIMER) snprintf(text, sizeof(text), "Timer %s is up", formatted);
    else snprintf(text, sizeof(text), "Alarm %.5s", formatted);
    if (process__is_background()) (void)notification__push(text, CLOCK_GUI__NOTIFICATION_MS);
    for (int beep = 0; beep < 3; ++beep) {
        (void)audio__tone(1500, 150, false);
        (void)runtime__delay(100);
    }
    if (!process__is_background()) (void)dialog__message(BRUCE_DIALOG_WARNING, "Clock", text);
    (void)input__flush();
}

static clock_scene_t clock_gui__scene(const clock_gui_t *gui, const clock_now_t *now) {
    return (clock_scene_t){
        .model = &gui->model,
        .local = now->valid ? &now->local : NULL,
        .now_ms = runtime__now(),
        .width = (int16_t)display__width(),
        .height = (int16_t)display__height(),
        .show_menu_hint = runtime__now() - gui->started_ms < CLOCK_GUI__MENU_HINT_MS,
    };
}

static void clock_gui__show_screen(clock_gui_t *gui, clock_screen_t screen, bool menu_open) {
    gui->screen = screen;
    gui->menu_open = menu_open;
    gui->ui.focus = 0;
    gui->ui.scroll = 0;
}

static void clock_gui__add_custom_timer(clock_gui_t *gui) {
    uint32_t seconds = 0;
    if (dialog__duration_input("Timer", &seconds) == BRUCE_OK) {
        (void)clock_model__add_timer(&gui->model, seconds, runtime__now());
    }
    (void)input__flush();
}

static void clock_gui__add_alarm(clock_gui_t *gui, const clock_now_t *now) {
    if (!now->valid) return;
    uint32_t seconds_of_day = now->local.hour * 3600u + now->local.minute * 60u;
    if (dialog__time_of_day_input("Alarm", &seconds_of_day) == BRUCE_OK) {
        (void)clock_model__add_alarm(&gui->model, seconds_of_day, now->seconds);
        clock_gui__save_alarms(&gui->model);
    }
    (void)input__flush();
}

/* Returns true when the app should exit. */
static bool clock_gui__close(clock_gui_t *gui) {
    if (!clock_model__is_active(&gui->model)) return true;
    (void)process__to_background();
    return false;
}

static bool clock_gui__activate(clock_gui_t *gui, const clock_item_t *item, const clock_now_t *now) {
    uint64_t now_ms = runtime__now();
    size_t index = (size_t)item->arg;
    switch ((clock_action_t)item->action) {
    case CLOCK_ACTION_GO_SCREEN: clock_gui__show_screen(gui, (clock_screen_t)item->arg, false); break;
    case CLOCK_ACTION_CLOSE: return clock_gui__close(gui);
    case CLOCK_ACTION_ADD_TIMER: (void)clock_model__add_timer(&gui->model, (uint32_t)item->arg, now_ms); break;
    case CLOCK_ACTION_CUSTOM_TIMER: clock_gui__add_custom_timer(gui); break;
    case CLOCK_ACTION_TOGGLE_TIMER: clock_model__toggle_timer(&gui->model, index, now_ms); break;
    case CLOCK_ACTION_REMOVE_TIMER: clock_model__remove_timer(&gui->model, index); break;
    case CLOCK_ACTION_ADD_ALARM: clock_gui__add_alarm(gui, now); break;
    case CLOCK_ACTION_TOGGLE_ALARM:
        clock_model__toggle_alarm(&gui->model, index, now->seconds);
        clock_gui__save_alarms(&gui->model);
        break;
    case CLOCK_ACTION_REMOVE_ALARM:
        clock_model__remove_alarm(&gui->model, index);
        clock_gui__save_alarms(&gui->model);
        break;
    case CLOCK_ACTION_STOPWATCH_TOGGLE: clock_stopwatch__toggle(&gui->model.stopwatch, now_ms); break;
    case CLOCK_ACTION_STOPWATCH_LAP: clock_stopwatch__lap(&gui->model.stopwatch, now_ms); break;
    case CLOCK_ACTION_STOPWATCH_RESET: clock_stopwatch__reset(&gui->model.stopwatch); break;
    }
    return false;
}

static bool clock_gui__on_back(clock_gui_t *gui) {
    if (gui->menu_open || gui->screen != CLOCK_SCREEN_FACE) {
        clock_gui__show_screen(gui, CLOCK_SCREEN_FACE, false);
        return false;
    }
    return clock_gui__close(gui);
}

static bool clock_gui__on_select(clock_gui_t *gui, const clock_now_t *now) {
    if (gui->screen == CLOCK_SCREEN_FACE && !gui->menu_open) {
        gui->menu_open = true;
        return false;
    }
    const clock_item_t *item = clock_ui__focused(&gui->ui);
    return item != NULL && clock_gui__activate(gui, item, now);
}

/* Returns true when the app should exit. */
static bool clock_gui__on_key(clock_gui_t *gui, int32_t code, const clock_now_t *now) {
    switch (code) {
    case BRUCE_INPUT_CODE_BACK: return clock_gui__on_back(gui);
    case BRUCE_INPUT_CODE_SELECT: return clock_gui__on_select(gui, now);
    case BRUCE_INPUT_CODE_PREV:
    case BRUCE_INPUT_CODE_LEFT: clock_ui__focus_step(&gui->ui, -1); break;
    case BRUCE_INPUT_CODE_NEXT:
    case BRUCE_INPUT_CODE_RIGHT: clock_ui__focus_step(&gui->ui, 1); break;
    case BRUCE_INPUT_CODE_UP: clock_ui__focus_row(&gui->ui, -1); break;
    case BRUCE_INPUT_CODE_DOWN: clock_ui__focus_row(&gui->ui, 1); break;
    default: break;
    }
    return false;
}

static void clock_gui__redraw_if_changed(clock_gui_t *gui, const clock_now_t *now) {
    if (process__is_background() && display__width() == 0) return;
    clock_scene_t scene = clock_gui__scene(gui, now);
    if (process__is_background()) clock_screens__build_preview(&gui->ui, &scene);
    else clock_screens__build(&gui->ui, &scene, gui->screen, gui->menu_open);

    uint32_t signature = clock_ui__signature(&gui->ui);
    bool moved = display__viewport_changed();
    if (gui->has_drawn && signature == gui->drawn_signature && !moved) return;
    if (clock_ui__draw(&gui->ui) != BRUCE_OK) return;
    gui->drawn_signature = signature;
    gui->has_drawn = true;
}

/* A minimised clock that is not tiled into the process preview has no viewport, so it badges the screen instead. */
static void clock_gui__update_overlay(clock_gui_t *gui) {
    clock_headline_t headline = clock_model__headline(&gui->model, runtime__now());
    bool hidden = process__is_background() && display__width() == 0;
    clock_overlay__update(&gui->overlay, &headline, hidden);
}

static uint32_t clock_gui__poll_ms(const clock_gui_t *gui) {
    bool stopwatch_visible = gui->screen == CLOCK_SCREEN_STOPWATCH && gui->model.stopwatch.running;
    return stopwatch_visible ? CLOCK_GUI__STOPWATCH_POLL_MS : CLOCK_GUI__IDLE_POLL_MS;
}

static clock_screen_t clock_gui__screen_for_headline(const clock_model_t *model) {
    switch (clock_model__headline(model, runtime__now()).kind) {
    case CLOCK_HEADLINE_STOPWATCH: return CLOCK_SCREEN_STOPWATCH;
    case CLOCK_HEADLINE_TIMER: return CLOCK_SCREEN_TIMERS;
    default: return CLOCK_SCREEN_FACE;
    }
}

/* Coming back from the background opens whatever was left running. */
static void clock_gui__follow_foreground(clock_gui_t *gui) {
    bool background = process__is_background();
    if (gui->backgrounded && !background) {
        clock_gui__show_screen(gui, clock_gui__screen_for_headline(&gui->model), false);
    }
    gui->backgrounded = background;
}

static int clock_gui__loop(clock_gui_t *gui) {
    for (;;) {
        clock_now_t now = clock_gui__now();
        clock_event_t event = clock_model__tick(&gui->model, runtime__now(), now.seconds);
        if (event.kind != CLOCK_EVENT_NONE) {
            clock_gui__announce(&event);
            continue;
        }
        clock_gui__follow_foreground(gui);
        clock_gui__update_overlay(gui);
        clock_gui__redraw_if_changed(gui, &now);

        bruce_input_event_t input;
        bruce_result_t result = input__read(&input, clock_gui__poll_ms(gui));
        if (result == BRUCE_ERR_NOT_FOREGROUND) {
            if (runtime__delay(CLOCK_GUI__IDLE_POLL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
        } else if (result == BRUCE_OK && input.action == BRUCE_INPUT_PRESS) {
            if (clock_gui__on_key(gui, input.code, &now)) return BRUCE_OK;
        } else if (result != BRUCE_OK && result != BRUCE_ERR_TIMEOUT) {
            return result;
        }
    }
}

int clock_gui__run(clock_screen_t first_screen) {
    clock_gui_t *gui = memory__calloc(1, sizeof(*gui));
    if (gui == NULL) return BRUCE_ERR_NO_MEMORY;
    gui->started_ms = runtime__now();
    clock_now_t now = clock_gui__now();
    clock_gui__load_alarms(&gui->model, &now);
    clock_gui__show_screen(gui, first_screen, false);
    (void)input__flush();
    int result = clock_gui__loop(gui);
    memory__free(gui);
    return result;
}
