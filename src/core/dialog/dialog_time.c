#include "dialog_time.h"

#include "dialog_gui_common.h"

#include <stdio.h>

#include "core_sdk/display.h"
#include "core_sdk/input.h"

#define DIALOG__CHAR_W 6
#define DIALOG__CHAR_H 10
#define DIALOG__GLYPH_TOP_ROWS 2
#define DIALOG__GLYPH_INK_ROWS 7
#define DIALOG__MARGIN 2
#define DIALOG__TIME_MAX_FIELDS 3
#define DIALOG__TIME_MAX_TEXT_SIZE 5
#define DIALOG__TIME_OK_TEXT_SIZE 2
#define DIALOG__TIME_FIELD_PADDING 3
#define DIALOG__TIME_ARROW_GAP 4

typedef struct {
    uint8_t values[DIALOG__TIME_MAX_FIELDS];
    uint8_t maxima[DIALOG__TIME_MAX_FIELDS];
    int field_count;
    int focus;
    bool editing;
} dialog__time_state_t;

typedef struct {
    int title_h;
    int text_size, char_w;
    int digits_x, ink_y, ink_w, ink_h;
    int ok_y, ok_h;
} dialog__time_layout_t;

typedef struct {
    uint16_t primary, secondary, background, text;
} dialog__time_colors_t;

static dialog__time_colors_t dialog__time_colors(void) {
    dialog__time_colors_t colors;
    uint16_t surface, muted, border, success, warning, error;
    dialog__get_colors(
        &colors.primary, &colors.secondary, &colors.background, &surface, &colors.text, &muted, &border,
        &success, &warning, &error
    );
    return colors;
}

static bool dialog__time_ok_focused(const dialog__time_state_t *st) { return st->focus == st->field_count; }

static void dialog__time_move_focus(dialog__time_state_t *st, int direction) {
    int slots = st->field_count + 1;
    st->focus = (st->focus + direction + slots) % slots;
    st->editing = false;
}

static void dialog__time_step(dialog__time_state_t *st, int direction) {
    if (dialog__time_ok_focused(st)) return;
    int range = st->maxima[st->focus] + 1;
    st->values[st->focus] = (uint8_t)((st->values[st->focus] + direction + range) % range);
}

static dialog__time_layout_t dialog__time_layout(const dialog__time_state_t *st) {
    int w = display__width(), h = display__height();
    int chars = st->field_count * 3 - 1;
    dialog__time_layout_t layout = {.title_h = DIALOG__CHAR_H + 4};
    layout.text_size = (w - 2 * DIALOG__MARGIN) / (DIALOG__CHAR_W * chars);
    if (layout.text_size > DIALOG__TIME_MAX_TEXT_SIZE) layout.text_size = DIALOG__TIME_MAX_TEXT_SIZE;
    if (layout.text_size < 1) layout.text_size = 1;
    layout.char_w = DIALOG__CHAR_W * layout.text_size;
    layout.digits_x = (w - chars * layout.char_w) / 2;
    layout.ink_w = 2 * layout.char_w - layout.text_size;
    layout.ink_h = DIALOG__GLYPH_INK_ROWS * layout.text_size;
    layout.ok_h = DIALOG__CHAR_H * DIALOG__TIME_OK_TEXT_SIZE + 8;
    layout.ok_y = h - layout.ok_h - 2 * DIALOG__MARGIN;
    layout.ink_y = layout.title_h + (layout.ok_y - layout.title_h - layout.ink_h) / 2;
    return layout;
}

static void dialog__time_draw_title(
    const char *title, const dialog__time_layout_t *layout, const dialog__time_colors_t *colors
) {
    display__fill_screen(colors->background);
    display__fill_rect(0, 0, display__width(), layout->title_h, colors->primary);
    display__set_text_color(colors->text);
    display__set_text_size(1);
    display__set_text_bg_color(colors->primary);
    display__set_cursor(DIALOG__MARGIN, DIALOG__MARGIN);
    display__print(title != NULL ? title : "");
}

static void dialog__time_draw_arrows(
    const dialog__time_layout_t *layout, int center_x, int top, int bottom, uint16_t color
) {
    int size = layout->text_size * 3;
    top -= DIALOG__TIME_ARROW_GAP;
    bottom += DIALOG__TIME_ARROW_GAP;
    display__fill_triangle(center_x - size, top, center_x + size, top, center_x, top - size, color);
    display__fill_triangle(center_x - size, bottom, center_x + size, bottom, center_x, bottom + size, color);
}

static void dialog__time_draw_field(
    const dialog__time_state_t *st, const dialog__time_layout_t *layout, const dialog__time_colors_t *colors,
    int field
) {
    bool focused = st->focus == field;
    int pad = DIALOG__TIME_FIELD_PADDING;
    int x = layout->digits_x + field * 3 * layout->char_w;
    if (focused) {
        uint16_t highlight = st->editing ? colors->secondary : colors->primary;
        int top = layout->ink_y - pad;
        int height = layout->ink_h + 2 * pad;
        display__fill_rect(x - pad, top, layout->ink_w + 2 * pad, height, highlight);
        if (st->editing) {
            dialog__time_draw_arrows(layout, x + layout->ink_w / 2, top, top + height, highlight);
        }
    }
    char digits[4];
    snprintf(digits, sizeof(digits), "%02u", (unsigned)st->values[field]);
    int glyph_y = layout->ink_y - DIALOG__GLYPH_TOP_ROWS * layout->text_size;
    display__set_text_size(layout->text_size);
    display__set_text_color(colors->text);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_cursor(x, glyph_y);
    display__print(digits);
    if (field < st->field_count - 1) {
        display__set_cursor(x + 2 * layout->char_w, glyph_y);
        display__print(":");
    }
}

static void dialog__time_draw_ok(
    const dialog__time_state_t *st, const dialog__time_layout_t *layout, const dialog__time_colors_t *colors
) {
    int size = DIALOG__TIME_OK_TEXT_SIZE;
    int w = display__width() - 4 * DIALOG__MARGIN;
    int x = 2 * DIALOG__MARGIN;
    display__fill_rect(
        x, layout->ok_y, w, layout->ok_h, dialog__time_ok_focused(st) ? colors->primary : colors->background
    );
    display__draw_rect(x, layout->ok_y, w, layout->ok_h, colors->primary);
    int ink_y = layout->ok_y + (layout->ok_h - DIALOG__GLYPH_INK_ROWS * size) / 2;
    int ink_w = 2 * DIALOG__CHAR_W * size - size;
    display__set_text_size(size);
    display__set_text_color(colors->text);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_cursor(x + (w - ink_w) / 2, ink_y - DIALOG__GLYPH_TOP_ROWS * size);
    display__print("OK");
}

static bruce_result_t dialog__time_draw(const dialog__time_state_t *st, const char *title) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    dialog__time_colors_t colors = dialog__time_colors();
    dialog__time_layout_t layout = dialog__time_layout(st);
    dialog__time_draw_title(title, &layout, &colors);
    for (int field = 0; field < st->field_count; ++field) {
        dialog__time_draw_field(st, &layout, &colors, field);
    }
    dialog__time_draw_ok(st, &layout, &colors);
    return display__present();
}

static void dialog__time_load(dialog__time_state_t *st, bool with_seconds, uint8_t max_hours, uint32_t seconds) {
    *st = (dialog__time_state_t){.field_count = with_seconds ? 3 : 2, .maxima = {max_hours, 59, 59}};
    uint32_t parts[DIALOG__TIME_MAX_FIELDS] = {seconds / 3600u, seconds / 60u % 60u, seconds % 60u};
    for (int field = 0; field < st->field_count; ++field) {
        st->values[field] = parts[field] > st->maxima[field] ? st->maxima[field] : (uint8_t)parts[field];
    }
}

static uint32_t dialog__time_total_seconds(const dialog__time_state_t *st) {
    uint32_t seconds = st->values[0] * 3600u + st->values[1] * 60u;
    return st->field_count == 3 ? seconds + st->values[2] : seconds;
}

bruce_result_t dialog__time_gui_run(const char *title, bool with_seconds, uint8_t max_hours, uint32_t *seconds) {
    dialog__time_state_t st;
    dialog__time_load(&st, with_seconds, max_hours, *seconds);
    (void)input__flush();
    bool redraw = true;
    for (;;) {
        if (redraw) {
            bruce_result_t draw_result = dialog__time_draw(&st, title);
            if (draw_result == BRUCE_ERR_NOT_FOREGROUND) return BRUCE_ERR_CANCELLED;
            if (draw_result != BRUCE_OK) return draw_result;
            redraw = false;
        }
        bruce_input_event_t ev;
        bruce_result_t input_result = input__read(&ev, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) return BRUCE_ERR_CANCELLED;
        if (input_result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) continue;

        redraw = true;
        switch (ev.code) {
            case BRUCE_INPUT_CODE_UP: dialog__time_step(&st, 1); break;
            case BRUCE_INPUT_CODE_DOWN: dialog__time_step(&st, -1); break;
            case BRUCE_INPUT_CODE_LEFT: dialog__time_move_focus(&st, -1); break;
            case BRUCE_INPUT_CODE_RIGHT: dialog__time_move_focus(&st, 1); break;
            case BRUCE_INPUT_CODE_PREV:
                if (st.editing) dialog__time_step(&st, -1);
                else dialog__time_move_focus(&st, -1);
                break;
            case BRUCE_INPUT_CODE_NEXT:
                if (st.editing) dialog__time_step(&st, 1);
                else dialog__time_move_focus(&st, 1);
                break;
            case BRUCE_INPUT_CODE_SELECT:
            case BRUCE_INPUT_CODE_BUTTON_A:
                if (dialog__time_ok_focused(&st)) {
                    *seconds = dialog__time_total_seconds(&st);
                    return BRUCE_OK;
                }
                st.editing = !st.editing;
                break;
            case BRUCE_INPUT_CODE_BACK:
            case BRUCE_INPUT_CODE_BUTTON_B: return BRUCE_ERR_CANCELLED;
            default: redraw = false;
        }
    }
}
