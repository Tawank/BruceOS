#include "process_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"

#define PROCESS_LIST__REFRESH_MS 1000u
#define PROCESS_LIST__PAD 3
#define PROCESS_LIST__TOUCH_ROW_H 24

/* Selectable rows: the buttons first, then process i at PROCESS_LIST__BUTTONS + i. */
enum { PROCESS_LIST__BUTTON_PREVIEW, PROCESS_LIST__BUTTON_EXIT, PROCESS_LIST__BUTTONS };

typedef struct {
    bruce_process_snapshot_t *processes;
    size_t count;
    size_t selected;
    size_t first_visible;
    bool show_selection; /* Hidden on touch screens until a navigation key is used. */
    bruce_memory_stats_t memory;
} process_list_state_t;

/* Screen geometry, shared by drawing and touch hit-testing. */
typedef struct {
    int char_w, char_h;
    int bar_h;   /* Title bar and column header. */
    int row_h;   /* Buttons and process rows; taller on touch screens. */
    int button_y, preview_w, exit_x;
    int header_y, list_top;
    size_t visible;
} process_list_layout_t;

static process_list_layout_t process_list__layout(void) {
    int16_t char_w = 6, char_h = 10;
    (void)display__get_font_metrics(&char_w, &char_h);
    process_list_layout_t l = {.char_w = char_w, .char_h = char_h, .bar_h = char_h + 3};
    l.row_h = l.bar_h;
    if (input__has_touch() && l.row_h < PROCESS_LIST__TOUCH_ROW_H) l.row_h = PROCESS_LIST__TOUCH_ROW_H;
    int w = display__width();
    l.button_y = l.bar_h + 3;
    l.preview_w = (w - 3 * PROCESS_LIST__PAD) * 2 / 3;
    l.exit_x = 2 * PROCESS_LIST__PAD + l.preview_w;
    l.header_y = l.button_y + l.row_h + 3;
    l.list_top = l.header_y + l.bar_h;
    int h = display__height();
    l.visible = h > l.list_top ? (size_t)((h - l.list_top) / l.row_h) : 0;
    return l;
}

static char process_list__state_letter(bruce_process_state_t state) {
    switch (state) {
        case BRUCE_PROCESS_FOREGROUND: return 'F';
        case BRUCE_PROCESS_BACKGROUND: return 'B';
        case BRUCE_PROCESS_PAUSED: return 'P';
        case BRUCE_PROCESS_STOPPING: return 'S';
        default: return '?';
    }
}

static void process_list__format_size(size_t bytes, char *out, size_t out_size) {
    if (bytes >= 10u * 1024u * 1024u) {
        snprintf(out, out_size, "%uM", (unsigned)(bytes / (1024u * 1024u)));
    } else if (bytes >= 1024u * 1024u) {
        unsigned tenths = (unsigned)(bytes * 10u / (1024u * 1024u));
        snprintf(out, out_size, "%u.%uM", tenths / 10u, tenths % 10u);
    } else {
        snprintf(out, out_size, "%uK", (unsigned)((bytes + 1023u) / 1024u));
    }
}

static const bruce_process_snapshot_t *process_list__selected_process(const process_list_state_t *state) {
    if (state->selected < PROCESS_LIST__BUTTONS || state->selected - PROCESS_LIST__BUTTONS >= state->count) {
        return NULL;
    }
    return &state->processes[state->selected - PROCESS_LIST__BUTTONS];
}

static void process_list__refresh(process_list_state_t *state) {
    const bruce_process_snapshot_t *selected = process_list__selected_process(state);
    bruce_process_id_t selected_id = selected != NULL ? selected->id : BRUCE_PROCESS_ID_INVALID;
    if (process__list(state->processes, BRUCE_PROCESS_MAX, &state->count) != BRUCE_OK) state->count = 0;
    (void)memory__get_stats(&state->memory);
    if (selected_id == BRUCE_PROCESS_ID_INVALID) return;
    for (size_t i = 0; i < state->count; ++i) {
        if (state->processes[i].id == selected_id) {
            state->selected = PROCESS_LIST__BUTTONS + i;
            return;
        }
    }
    size_t last = PROCESS_LIST__BUTTONS + state->count - 1;
    if (state->selected > last) state->selected = last;
}

static void process_list__draw_button(const char *label, int x, int y, int w, int h, bool selected) {
    uint16_t pri = config__get_color_primary();
    uint16_t bg = config__get_color_background();
    int16_t char_w = 6, char_h = 10;
    (void)display__get_font_metrics(&char_w, &char_h);
    if (selected) display__fill_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, pri);
    else display__draw_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, pri);
    display__set_text_color(selected ? bg : pri);
    display__set_text_bg_color(selected ? pri : bg);
    display__draw_string(
        label, (int16_t)(x + (w - (int)strlen(label) * char_w) / 2), (int16_t)(y + (h - char_h) / 2 + 1)
    );
}

static void process_list__print_right(const char *text, int right, int y) {
    int16_t char_w = 6, char_h = 10;
    (void)display__get_font_metrics(&char_w, &char_h);
    display__draw_string(text, (int16_t)(right - (int)strlen(text) * char_w), (int16_t)y);
}

/* One table row; `name_chars` is how much of the name fits between PID and the numbers. */
static void process_list__draw_row(
    const bruce_process_snapshot_t *process, int y, int w, int char_w, size_t name_chars, uint16_t fg,
    uint16_t bg
) {
    char text[BRUCE_PROCESS_NAME_MAX + 8];
    display__set_text_color(fg);
    display__set_text_bg_color(bg);
    char state = process_list__state_letter(process->state);
    snprintf(text, sizeof(text), "%3u %c ", (unsigned)process->id, state);
    display__draw_string(text, PROCESS_LIST__PAD, (int16_t)y);
    snprintf(text, sizeof(text), "%.*s", (int)name_chars, process->name);
    display__draw_string(text, (int16_t)(PROCESS_LIST__PAD + 6 * char_w), (int16_t)y);
    char cpu[8];
    char mem[8];
    snprintf(cpu, sizeof(cpu), "%u%%", (unsigned)process->cpu_percent);
    process_list__format_size(process->memory_bytes, mem, sizeof(mem));
    process_list__print_right(mem, w - PROCESS_LIST__PAD, y);
    process_list__print_right(cpu, w - PROCESS_LIST__PAD - 7 * char_w, y);
}

static bruce_result_t process_list__draw(process_list_state_t *state) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;

    uint16_t pri = config__get_color_primary();
    uint16_t bg = config__get_color_background();
    uint16_t text = config__get_color_text();
    uint16_t muted = config__get_color_text_muted();
    int w = display__width();
    process_list_layout_t l = process_list__layout();
    int char_w = l.char_w;
    char line[48];
    char internal[8];
    char psram[8];

    display__fill_screen(bg);
    display__set_text_size(1);

    display__fill_rect(0, 0, (int16_t)w, (int16_t)l.bar_h, pri);
    display__set_text_color(bg);
    display__set_text_bg_color(pri);
    snprintf(line, sizeof(line), "Tasks %u", (unsigned)state->count);
    display__draw_string(line, PROCESS_LIST__PAD, 2);
    process_list__format_size(state->memory.internal_free, internal, sizeof(internal));
    process_list__format_size(state->memory.psram_free, psram, sizeof(psram));
    snprintf(line, sizeof(line), "free int %s psram %s", internal, psram);
    process_list__print_right(line, w - PROCESS_LIST__PAD, 2);

    bool show = state->show_selection;
    process_list__draw_button(
        "Process preview", PROCESS_LIST__PAD, l.button_y, l.preview_w, l.row_h,
        show && state->selected == PROCESS_LIST__BUTTON_PREVIEW
    );
    int exit_w = w - PROCESS_LIST__PAD - l.exit_x;
    process_list__draw_button(
        "Exit", l.exit_x, l.button_y, exit_w, l.row_h, show && state->selected == PROCESS_LIST__BUTTON_EXIT
    );

    display__set_text_color(muted);
    display__set_text_bg_color(bg);
    display__draw_string("PID S NAME", PROCESS_LIST__PAD, (int16_t)l.header_y);
    process_list__print_right("MEM", w - PROCESS_LIST__PAD, l.header_y);
    process_list__print_right("CPU", w - PROCESS_LIST__PAD - 7 * char_w, l.header_y);
    int16_t rule_y = (int16_t)(l.header_y + l.bar_h - 2);
    display__draw_line(0, rule_y, (int16_t)(w - 1), rule_y, muted);

    size_t visible = l.visible;
    if (show && state->selected >= PROCESS_LIST__BUTTONS) {
        size_t row = state->selected - PROCESS_LIST__BUTTONS;
        if (row < state->first_visible) state->first_visible = row;
        if (visible > 0 && row >= state->first_visible + visible) state->first_visible = row - visible + 1;
    }
    size_t max_first = state->count > visible ? state->count - visible : 0;
    if (state->first_visible > max_first) state->first_visible = max_first;

    int numbers_w = 14 * char_w;
    int name_w = w - 2 * PROCESS_LIST__PAD - 6 * char_w - numbers_w;
    size_t name_chars = name_w > 0 ? (size_t)(name_w / char_w) : 0;
    for (size_t i = 0; i < visible && state->first_visible + i < state->count; ++i) {
        size_t index = state->first_visible + i;
        int y = l.list_top + (int)i * l.row_h;
        bool selected = show && state->selected == PROCESS_LIST__BUTTONS + index;
        if (selected) display__fill_rect(0, (int16_t)(y - 1), (int16_t)w, (int16_t)l.row_h, pri);
        process_list__draw_row(
            &state->processes[index], y + (l.row_h - l.char_h) / 2, w, char_w, name_chars, selected ? bg : text,
            selected ? pri : bg
        );
    }
    return display__present();
}

/* The row under a tap, as a `selected` value, or -1 for none. */
static int process_list__hit(const process_list_state_t *state, int x, int y) {
    process_list_layout_t l = process_list__layout();
    if (y >= l.button_y && y < l.button_y + l.row_h) {
        return x < l.exit_x ? PROCESS_LIST__BUTTON_PREVIEW : PROCESS_LIST__BUTTON_EXIT;
    }
    if (y < l.list_top) return -1;
    size_t row = state->first_visible + (size_t)((y - l.list_top) / l.row_h);
    return row < state->count ? (int)(PROCESS_LIST__BUTTONS + row) : -1;
}

/* Swipe up shows later rows, swipe down earlier ones, half a screen at a time. */
static void process_list__scroll(process_list_state_t *state, int direction) {
    size_t step = process_list__layout().visible / 2;
    if (step == 0) step = 1;
    if (direction > 0) state->first_visible += step;
    else state->first_visible = state->first_visible > step ? state->first_visible - step : 0;
}

static void process_list__print(void) {
    bruce_process_snapshot_t *processes = memory__malloc(BRUCE_PROCESS_MAX * sizeof(*processes));
    if (processes == NULL) return;
    size_t count = 0;
    if (process__list(processes, BRUCE_PROCESS_MAX, &count) == BRUCE_OK) {
        printf("PID S  CPU    MEM  STACK NAME\n");
        for (size_t i = 0; i < count; ++i) {
            const bruce_process_snapshot_t *p = &processes[i];
            char mem[8];
            char stack[8];
            process_list__format_size(p->memory_bytes, mem, sizeof(mem));
            process_list__format_size(p->stack_total_bytes, stack, sizeof(stack));
            printf(
                "%3u %c %3u%% %6s %6s %s\n", (unsigned)p->id, process_list__state_letter(p->state),
                (unsigned)p->cpu_percent, mem, stack, p->name
            );
        }
    }
    memory__free(processes);
}

static bool process_list__can_switch(const bruce_process_snapshot_t *process) {
    return process->id != process__current_id() && process->gui_requested &&
           process->state == BRUCE_PROCESS_BACKGROUND;
}

/* Returns true when the list should close: Exit, or the user switched to another app. */
static bool process_list__activate(process_list_state_t *state) {
    if (state->selected == PROCESS_LIST__BUTTON_EXIT) return true;
    if (state->selected == PROCESS_LIST__BUTTON_PREVIEW) {
        (void)process_app__preview();
        return process__is_background();
    }
    const bruce_process_snapshot_t *selected = process_list__selected_process(state);
    if (selected == NULL) return false;
    bruce_process_snapshot_t target = *selected;
    if (!process_app__menu(&target, process_list__can_switch(&target))) return false;
    return process__foreground(target.id) == BRUCE_OK;
}

static int process_list__gui(void) {
    process_list_state_t state = {
        .processes = memory__malloc(BRUCE_PROCESS_MAX * sizeof(*state.processes)),
        .show_selection = !input__has_touch(),
    };
    bruce_input_touch_t touch = {0};
    if (state.processes == NULL) return BRUCE_ERR_NO_MEMORY;
    uint64_t next_refresh = 0;
    bool dirty = true;
    int result = 0;
    (void)input__flush();
    for (;;) {
        uint64_t now = runtime__now();
        if (now >= next_refresh) {
            process_list__refresh(&state);
            next_refresh = now + PROCESS_LIST__REFRESH_MS;
            dirty = true;
        }
        if (dirty) {
            bruce_result_t draw = process_list__draw(&state);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            if (draw == BRUCE_ERR_NOT_INITIALIZED) {
                result = draw;
                break;
            }
            dirty = false;
        }

        bruce_input_event_t event;
        uint64_t wait = next_refresh > now ? next_refresh - now : 0;
        bruce_result_t input_result = input__read(&event, (uint32_t)wait);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            if (display__width() > 0) {
                if (runtime__delay(200) != BRUCE_OK) break;
            } else if (!display__await_redraw() && runtime__delay(20) != BRUCE_OK) {
                break;
            }
            if (display__viewport_changed()) dirty = true;
            continue;
        }
        if (input_result != BRUCE_OK) continue;
        if (event.type == BRUCE_INPUT_TOUCH) {
            bruce_input_gesture_t gesture = input__touch_gesture(&touch, &event);
            if (gesture == BRUCE_INPUT_GESTURE_SWIPE_UP || gesture == BRUCE_INPUT_GESTURE_SWIPE_DOWN) {
                process_list__scroll(&state, gesture == BRUCE_INPUT_GESTURE_SWIPE_UP ? 1 : -1);
                state.show_selection = false;
                dirty = true;
            } else if (gesture == BRUCE_INPUT_GESTURE_TAP || gesture == BRUCE_INPUT_GESTURE_HOLD) {
                int hit = process_list__hit(&state, touch.x, touch.y);
                if (hit < 0) continue;
                state.selected = (size_t)hit;
                state.show_selection = false;
                if (process_list__activate(&state)) break;
                next_refresh = 0;
            }
            continue;
        }
        if (event.action != BRUCE_INPUT_PRESS) continue;
        if (!state.show_selection) {
            state.show_selection = true;
            dirty = true;
        }

        size_t total = PROCESS_LIST__BUTTONS + state.count;
        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B) break;
        if (event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_PREV) {
            state.selected = (state.selected + total - 1) % total;
            dirty = true;
        } else if (event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT) {
            state.selected = (state.selected + 1) % total;
            dirty = true;
        } else if (event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A) {
            if (process_list__activate(&state)) break;
            next_refresh = 0;
        }
    }
    memory__free(state.processes);
    return result;
}

int process_list__run(void) {
    if (runtime__is_gui_requested()) return process_list__gui();
    process_list__print();
    return 0;
}
