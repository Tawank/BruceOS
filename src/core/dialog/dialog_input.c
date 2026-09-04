#include "dialog_input.h"

#include "core/config/config.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define DIALOG__CHAR_W 6
#define DIALOG__CHAR_H 10
#define DIALOG__TEXT_SIZE 1
#define DIALOG__MARGIN 2

enum { DIALOG__KEY_OK = 1, DIALOG__KEY_CANCEL, DIALOG__KEY_DELETE, DIALOG__KEY_SPACE, DIALOG__KEY_CAPS };

typedef struct {
    const char *label;
    char code;
    int special;
} dialog__key_t;

static const dialog__key_t s_text_keys[5][10] = {
    {{"1", '1', 0},
     {"2", '2', 0},
     {"3", '3', 0},
     {"4", '4', 0},
     {"5", '5', 0},
     {"6", '6', 0},
     {"7", '7', 0},
     {"8", '8', 0},
     {"9", '9', 0},
     {"0", '0', 0}},
    {{"q", 'q', 0},
     {"w", 'w', 0},
     {"e", 'e', 0},
     {"r", 'r', 0},
     {"t", 't', 0},
     {"y", 'y', 0},
     {"u", 'u', 0},
     {"i", 'i', 0},
     {"o", 'o', 0},
     {"p", 'p', 0}},
    {{"a", 'a', 0},
     {"s", 's', 0},
     {"d", 'd', 0},
     {"f", 'f', 0},
     {"g", 'g', 0},
     {"h", 'h', 0},
     {"j", 'j', 0},
     {"k", 'k', 0},
     {"l", 'l', 0},
     {";", ';', 0}},
    {{"z", 'z', 0},
     {"x", 'x', 0},
     {"c", 'c', 0},
     {"v", 'v', 0},
     {"b", 'b', 0},
     {"n", 'n', 0},
     {"m", 'm', 0},
     {",", ',', 0},
     {".", '.', 0},
     {"/", '/', 0}},
    {{"OK", 0, DIALOG__KEY_OK},
     {"AB", 0, DIALOG__KEY_CAPS},
     {"<-", 0, DIALOG__KEY_DELETE},
     {"SP", 0, DIALOG__KEY_SPACE},
     {"X", 0, DIALOG__KEY_CANCEL}},
};

static const dialog__key_t s_hex_keys[5][4] = {
    {{"0", '0', 0}, {"1", '1', 0}, {"2", '2', 0}, {"3", '3', 0}},
    {{"4", '4', 0}, {"5", '5', 0}, {"6", '6', 0}, {"7", '7', 0}},
    {{"8", '8', 0}, {"9", '9', 0}, {"A", 'A', 0}, {"B", 'B', 0}},
    {{"C", 'C', 0}, {"D", 'D', 0}, {"E", 'E', 0}, {"F", 'F', 0}},
    {{"OK", 0, DIALOG__KEY_OK}, {"DEL", 0, DIALOG__KEY_DELETE}, {"CAN", 0, DIALOG__KEY_CANCEL}},
};

static const dialog__key_t s_num_keys[5][3] = {
    {{"1", '1', 0},             {"2", '2', 0},                  {"3", '3', 0}                 },
    {{"4", '4', 0},             {"5", '5', 0},                  {"6", '6', 0}                 },
    {{"7", '7', 0},             {"8", '8', 0},                  {"9", '9', 0}                 },
    {{"-", '-', 0},             {"0", '0', 0},                  {".", '.', 0}                 },
    {{"OK", 0, DIALOG__KEY_OK}, {"DEL", 0, DIALOG__KEY_DELETE}, {"CAN", 0, DIALOG__KEY_CANCEL}},
};

typedef struct {
    const dialog__key_t *keys;
    int rows, cols, sel_row, sel_col;
    bool caps, mask_input;
    char *buffer;
    size_t buffer_size, len, max_len;
    const char *title, *prompt;
} dialog__keyboard_state_t;

static bool dialog__key_valid(const dialog__key_t *key) { return key != NULL && key->label != NULL; }

static const dialog__key_t *dialog__key_current(const dialog__keyboard_state_t *st) {
    if (st->sel_row < 0 || st->sel_row >= st->rows || st->sel_col < 0 || st->sel_col >= st->cols) return NULL;
    return &st->keys[st->sel_row * st->cols + st->sel_col];
}

static void
dialog__key_move_vertical_or_horizontal(dialog__keyboard_state_t *st, int row_delta, int col_delta) {
    int start_row = st->sel_row, start_col = st->sel_col;
    for (int attempt = 0; attempt < st->rows * st->cols; ++attempt) {
        st->sel_row = (st->sel_row + row_delta + st->rows) % st->rows;
        st->sel_col = (st->sel_col + col_delta + st->cols) % st->cols;
        if (dialog__key_valid(dialog__key_current(st))) return;
    }
    st->sel_row = start_row;
    st->sel_col = start_col;
}

/* PREV/NEXT visit valid keys in visual reading order, including partial rows. */
static void dialog__key_move_linear(dialog__keyboard_state_t *st, int direction) {
    int start = st->sel_row * st->cols + st->sel_col;
    int cells = st->rows * st->cols;
    for (int attempt = 1; attempt <= cells; ++attempt) {
        int index = (start + direction * attempt + cells) % cells;
        st->sel_row = index / st->cols;
        st->sel_col = index % st->cols;
        if (dialog__key_valid(dialog__key_current(st))) return;
    }
}

static void dialog__key_add(dialog__keyboard_state_t *st, char c) {
    if (st->len + 1 >= st->buffer_size || st->len >= st->max_len) return;
    st->buffer[st->len++] = c;
    st->buffer[st->len] = '\0';
}

static bool dialog__key_validate(const dialog__keyboard_state_t *st, char c, dialog__input_kind_t kind) {
    if (kind == DIALOG__INPUT_TEXT) return (c >= 0x20 && c <= 0x7e) || c == ' ';
    if (kind == DIALOG__INPUT_HEX) return isxdigit((unsigned char)c) != 0;
    if (c == '-') return st->len == 0;
    if (c == '.') {
        for (size_t i = 0; i < st->len; ++i)
            if (st->buffer[i] == '.') return false;
        return true;
    }
    return isdigit((unsigned char)c) != 0;
}

static void dialog__input_clear_and_title(const char *title) {
    uint16_t pri, sec, bg, surface, text, muted, border, success, warning, error;
    config__get_colors_internal(
        &pri, &sec, &bg, &surface, &text, &muted, &border, &success, &warning, &error
    );
    (void)display__fill_screen(bg);
    display__fill_rect(0, 0, display__width(), DIALOG__CHAR_H + 4, pri);
    display__set_text_color(text);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(pri);
    display__set_cursor(DIALOG__MARGIN, DIALOG__MARGIN);
    display__print(title != NULL ? title : "");
}

static uint16_t dialog__input_background_color(void) {
    uint16_t pri, sec, bg, surface, text, muted, border, success, warning, error;
    config__get_colors_internal(
        &pri, &sec, &bg, &surface, &text, &muted, &border, &success, &warning, &error
    );
    return bg;
}

static void
dialog__keyboard_layout(const dialog__keyboard_state_t *st, int *text_area_h, int *cell_w, int *cell_h) {
    int w = display__width(), h = display__height();
    *text_area_h = DIALOG__CHAR_H * 5 + 12;
    *cell_w = w / st->cols;
    *cell_h = (h - *text_area_h) / st->rows;
    if (*cell_w < 1) *cell_w = 1;
    if (*cell_h < 1) *cell_h = 1;
}

static void dialog__keyboard_draw_cell(
    const dialog__keyboard_state_t *st, dialog__input_kind_t kind, int row, int col, int text_area_h,
    int cell_w, int cell_h, uint16_t bg
) {
    const dialog__key_t *key = &st->keys[row * st->cols + col];
    if (!dialog__key_valid(key)) return;
    int x = col * cell_w, y = text_area_h + row * cell_h;
    bool selected = row == st->sel_row && col == st->sel_col;
    const char *label = key->label;
    char upper[2];
    if (!key->special && kind == DIALOG__INPUT_TEXT && st->caps && isalpha((unsigned char)key->code)) {
        upper[0] = (char)toupper((unsigned char)key->code);
        upper[1] = '\0';
        label = upper;
    }
    display__fill_rect(x, y, cell_w, cell_h, selected ? BRUCE_COLOR_WHITE : bg);
    if (selected) {
        display__set_text_color(BRUCE_COLOR_BLACK);
    } else {
        display__draw_rect(x, y, cell_w, cell_h, BRUCE_COLOR_WHITE);
        display__set_text_color(BRUCE_COLOR_WHITE);
    }
    int label_x = x + (cell_w - (int)strlen(label) * DIALOG__CHAR_W) / 2;
    int label_y = y + (cell_h - DIALOG__CHAR_H) / 2;
    if (label_x < x + 1) label_x = x + 1;
    if (label_y < y + 1) label_y = y + 1;
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_cursor(label_x, label_y);
    display__print(label);
}

static bruce_result_t dialog__keyboard_draw(dialog__keyboard_state_t *st, dialog__input_kind_t kind) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    int w = display__width(), text_area_h, cell_w, cell_h;
    dialog__keyboard_layout(st, &text_area_h, &cell_w, &cell_h);
    dialog__input_clear_and_title(st->title);
    display__set_text_color(BRUCE_COLOR_WHITE);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_cursor(DIALOG__MARGIN, DIALOG__CHAR_H + 8);
    display__print(st->prompt != NULL ? st->prompt : "");
    char counter[32];
    snprintf(counter, sizeof(counter), "%zu/%zu", st->len, st->max_len);
    display__set_cursor(w - (int)strlen(counter) * DIALOG__CHAR_W - DIALOG__MARGIN, DIALOG__CHAR_H + 8);
    display__print(counter);
    int textbox_y = DIALOG__CHAR_H * 2 + 10;
    display__draw_rect(
        DIALOG__MARGIN, textbox_y, w - 2 * DIALOG__MARGIN, DIALOG__CHAR_H * 2 + 4, BRUCE_COLOR_WHITE
    );
    display__set_cursor(DIALOG__MARGIN + 2, textbox_y + 2);
    if (st->mask_input) {
        char stars[64];
        size_t n = st->len < sizeof(stars) - 1 ? st->len : sizeof(stars) - 1;
        memset(stars, '*', n);
        stars[n] = '\0';
        display__print(stars);
    } else {
        int max_chars = (w - 4 * DIALOG__MARGIN) / DIALOG__CHAR_W;
        if (max_chars < 1) max_chars = 1;
        display__print((int)st->len <= max_chars ? st->buffer : st->buffer + st->len - max_chars);
    }
    uint16_t bg = dialog__input_background_color();
    for (int row = 0; row < st->rows; ++row)
        for (int col = 0; col < st->cols; ++col)
            dialog__keyboard_draw_cell(st, kind, row, col, text_area_h, cell_w, cell_h, bg);
    return display__present();
}

static bruce_result_t dialog__keyboard_draw_selection(
    dialog__keyboard_state_t *st, dialog__input_kind_t kind, int previous_row, int previous_col
) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    int text_area_h, cell_w, cell_h;
    dialog__keyboard_layout(st, &text_area_h, &cell_w, &cell_h);
    uint16_t bg = dialog__input_background_color();
    dialog__keyboard_draw_cell(st, kind, previous_row, previous_col, text_area_h, cell_w, cell_h, bg);
    dialog__keyboard_draw_cell(st, kind, st->sel_row, st->sel_col, text_area_h, cell_w, cell_h, bg);
    return display__present();
}

bruce_result_t dialog__input_gui_run(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size, dialog__input_kind_t kind
) {
    const dialog__key_t *keys;
    int rows, cols;
    if (kind == DIALOG__INPUT_TEXT) {
        keys = &s_text_keys[0][0];
        rows = 5;
        cols = 10;
    } else if (kind == DIALOG__INPUT_HEX) {
        keys = &s_hex_keys[0][0];
        rows = 5;
        cols = 4;
    } else {
        keys = &s_num_keys[0][0];
        rows = 5;
        cols = 3;
    }
    dialog__keyboard_state_t st = {
        .keys = keys,
        .rows = rows,
        .cols = cols,
        .buffer = buffer,
        .buffer_size = buffer_size,
        .max_len = buffer_size > 0 ? buffer_size - 1 : 0,
        .mask_input = mask_input,
        .title = title,
        .prompt = prompt
    };
    if (buffer_size > 0) buffer[0] = '\0';
    if (initial_text != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "%s", initial_text);
        st.len = strlen(buffer);
    }
    if (st.len > st.max_len) {
        st.len = st.max_len;
        buffer[st.len] = '\0';
    }
    (void)input__flush();
    bool full_redraw = true;
    for (;;) {
        if (full_redraw) {
            bruce_result_t draw_result = dialog__keyboard_draw(&st, kind);
            if (draw_result == BRUCE_ERR_NOT_FOREGROUND) return BRUCE_ERR_CANCELLED;
            if (draw_result != BRUCE_OK) return draw_result;
            full_redraw = false;
        }
        bruce_input_event_t ev;
        bruce_result_t input_result = input__read(&ev, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) return BRUCE_ERR_CANCELLED;
        if (input_result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) continue;
        if (ev.type == BRUCE_INPUT_KEY && ev.code == '\n') return BRUCE_OK;
        switch (ev.code) {
            case BRUCE_INPUT_CODE_UP:
            case BRUCE_INPUT_CODE_DOWN:
            case BRUCE_INPUT_CODE_PREV:
            case BRUCE_INPUT_CODE_NEXT:
            case BRUCE_INPUT_CODE_LEFT:
            case BRUCE_INPUT_CODE_RIGHT: {
                int previous_row = st.sel_row, previous_col = st.sel_col;
                if (ev.code == BRUCE_INPUT_CODE_UP) dialog__key_move_vertical_or_horizontal(&st, -1, 0);
                else if (ev.code == BRUCE_INPUT_CODE_DOWN) dialog__key_move_vertical_or_horizontal(&st, 1, 0);
                else if (ev.code == BRUCE_INPUT_CODE_PREV) dialog__key_move_linear(&st, -1);
                else if (ev.code == BRUCE_INPUT_CODE_NEXT) dialog__key_move_linear(&st, 1);
                else if (ev.code == BRUCE_INPUT_CODE_LEFT)
                    dialog__key_move_vertical_or_horizontal(&st, 0, -1);
                else dialog__key_move_vertical_or_horizontal(&st, 0, 1);
                bruce_result_t draw_result =
                    dialog__keyboard_draw_selection(&st, kind, previous_row, previous_col);
                if (draw_result == BRUCE_ERR_NOT_FOREGROUND) return BRUCE_ERR_CANCELLED;
                if (draw_result != BRUCE_OK) return draw_result;
                continue;
            }
            case BRUCE_INPUT_CODE_SELECT:
            case BRUCE_INPUT_CODE_BUTTON_A: {
                const dialog__key_t *key = dialog__key_current(&st);
                if (!dialog__key_valid(key)) continue;
                if (key->special == DIALOG__KEY_OK) return BRUCE_OK;
                if (key->special == DIALOG__KEY_CANCEL) return BRUCE_ERR_CANCELLED;
                if (key->special == DIALOG__KEY_DELETE) {
                    if (st.len > 0) buffer[--st.len] = '\0';
                } else if (key->special == DIALOG__KEY_SPACE) {
                    if (kind == DIALOG__INPUT_TEXT) dialog__key_add(&st, ' ');
                } else if (key->special == DIALOG__KEY_CAPS) st.caps = !st.caps;
                else if (key->code != '\0') {
                    char c = key->code;
                    if (kind == DIALOG__INPUT_TEXT && st.caps && isalpha((unsigned char)c))
                        c = (char)toupper((unsigned char)c);
                    if (dialog__key_validate(&st, c, kind)) dialog__key_add(&st, c);
                }
                full_redraw = true;
                continue;
            }
            case BRUCE_INPUT_CODE_BACK:
            case BRUCE_INPUT_CODE_BUTTON_B: return BRUCE_ERR_CANCELLED;
            case '\b':
            case 0x7f:
            case BRUCE_INPUT_CODE_DELETE:
                if (st.len > 0) buffer[--st.len] = '\0';
                full_redraw = true;
                continue;
            default: break;
        }
        if (kind == DIALOG__INPUT_TEXT && ev.type == BRUCE_INPUT_KEY && ev.code >= 0x20 && ev.code <= 0x7e) {
            dialog__key_add(&st, (char)ev.code);
            full_redraw = true;
            continue;
        }
        if (kind == DIALOG__INPUT_HEX && ev.type == BRUCE_INPUT_KEY && isxdigit((unsigned char)ev.code)) {
            dialog__key_add(&st, (char)ev.code);
            full_redraw = true;
            continue;
        }
        if (kind == DIALOG__INPUT_NUMBER && ev.type == BRUCE_INPUT_KEY &&
            dialog__key_validate(&st, (char)ev.code, kind)) {
            dialog__key_add(&st, (char)ev.code);
            full_redraw = true;
        }
    }
}
