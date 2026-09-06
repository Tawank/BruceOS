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

enum {
    DIALOG__KEY_OK = 1,
    DIALOG__KEY_CANCEL,
    DIALOG__KEY_DELETE,
    DIALOG__KEY_SPACE,
    DIALOG__KEY_CAPS,
    DIALOG__KEY_DIACRITICS,
    DIALOG__KEY_CURSOR_LEFT,
    DIALOG__KEY_CURSOR_RIGHT,
};

typedef struct {
    const char *label;
    char code;
    int special;
} dialog__key_t;

#define DIALOG__KEY(label, code) {label, code, 0}

static const dialog__key_t s_text_keys[5][12] = {
    {DIALOG__KEY("1", '1'),
     DIALOG__KEY("2", '2'),
     DIALOG__KEY("3", '3'),
     DIALOG__KEY("4", '4'),
     DIALOG__KEY("5", '5'),
     DIALOG__KEY("6", '6'),
     DIALOG__KEY("7", '7'),
     DIALOG__KEY("8", '8'),
     DIALOG__KEY("9", '9'),
     DIALOG__KEY("0", '0'),
     DIALOG__KEY("-", '-'),
     DIALOG__KEY("=", '=')},
    {DIALOG__KEY("q", 'q'),
     DIALOG__KEY("w", 'w'),
     DIALOG__KEY("e", 'e'),
     DIALOG__KEY("r", 'r'),
     DIALOG__KEY("t", 't'),
     DIALOG__KEY("y", 'y'),
     DIALOG__KEY("u", 'u'),
     DIALOG__KEY("i", 'i'),
     DIALOG__KEY("o", 'o'),
     DIALOG__KEY("p", 'p'),
     DIALOG__KEY("[", '['),
     DIALOG__KEY("]", ']')},
    {DIALOG__KEY("a", 'a'),
     DIALOG__KEY("s", 's'),
     DIALOG__KEY("d", 'd'),
     DIALOG__KEY("f", 'f'),
     DIALOG__KEY("g", 'g'),
     DIALOG__KEY("h", 'h'),
     DIALOG__KEY("j", 'j'),
     DIALOG__KEY("k", 'k'),
     DIALOG__KEY("l", 'l'),
     DIALOG__KEY(";", ';'),
     DIALOG__KEY("\"", '"'),
     DIALOG__KEY("|", '|')},
    {DIALOG__KEY("\\", '\\'),
     DIALOG__KEY("z", 'z'),
     DIALOG__KEY("x", 'x'),
     DIALOG__KEY("c", 'c'),
     DIALOG__KEY("v", 'v'),
     DIALOG__KEY("b", 'b'),
     DIALOG__KEY("n", 'n'),
     DIALOG__KEY("m", 'm'),
     DIALOG__KEY(",", ','),
     DIALOG__KEY(".", '.'),
     DIALOG__KEY("?", '?'),
     DIALOG__KEY("/", '/')},
    {{"OK", 0, DIALOG__KEY_OK},
     {NULL, 0, 0},
     {"A@", 0, DIALOG__KEY_CAPS},
     {NULL, 0, 0},
     {"ąć", 0, DIALOG__KEY_DIACRITICS},
     {NULL, 0, 0},
     {"__", 0, DIALOG__KEY_SPACE},
     {NULL, 0, 0},
     {"<", 0, DIALOG__KEY_CURSOR_LEFT},
     {">", 0, DIALOG__KEY_CURSOR_RIGHT},
     {"<-", 0, DIALOG__KEY_DELETE},
     {"X", 0, DIALOG__KEY_CANCEL}},
};

static const dialog__key_t s_diacritic_keys[5][12] = {
    {DIALOG__KEY("á", 0),
     DIALOG__KEY("à", 0),
     DIALOG__KEY("â", 0),
     DIALOG__KEY("ã", 0),
     DIALOG__KEY("ä", 0),
     DIALOG__KEY("å", 0),
     DIALOG__KEY("ç", 0),
     DIALOG__KEY("ę", 0),
     DIALOG__KEY("é", 0),
     DIALOG__KEY("è", 0),
     DIALOG__KEY("ê", 0),
     DIALOG__KEY("ë", 0)},
    {DIALOG__KEY("í", 0),
     DIALOG__KEY("ì", 0),
     DIALOG__KEY("î", 0),
     DIALOG__KEY("ï", 0),
     DIALOG__KEY("ñ", 0),
     DIALOG__KEY("ó", 0),
     DIALOG__KEY("ò", 0),
     DIALOG__KEY("ô", 0),
     DIALOG__KEY("õ", 0),
     DIALOG__KEY("ö", 0),
     DIALOG__KEY("ú", 0),
     DIALOG__KEY("ù", 0)},
    {DIALOG__KEY("û", 0),
     DIALOG__KEY("ü", 0),
     DIALOG__KEY("ý", 0),
     DIALOG__KEY("ś", 0),
     DIALOG__KEY("ß", 0),
     DIALOG__KEY("ø", 0),
     DIALOG__KEY("ł", 0),
     DIALOG__KEY("ń", 0),
     DIALOG__KEY("ź", 0),
     DIALOG__KEY("ż", 0),
     DIALOG__KEY("đ", 0),
     DIALOG__KEY("ň", 0)},
    {DIALOG__KEY("ă", 0),
     DIALOG__KEY("ą", 0),
     DIALOG__KEY("ć", 0),
     DIALOG__KEY("č", 0),
     DIALOG__KEY("ď", 0),
     DIALOG__KEY("ğ", 0),
     DIALOG__KEY("ı", 0),
     DIALOG__KEY("ŕ", 0),
     DIALOG__KEY("ě", 0),
     DIALOG__KEY("ţ", 0),
     DIALOG__KEY("ř", 0),
     DIALOG__KEY("♥", 0)},
    {{"OK", 0, DIALOG__KEY_OK},
     {NULL, 0, 0},
     {"A@", 0, DIALOG__KEY_CAPS},
     {NULL, 0, 0},
     {"ąć", 0, DIALOG__KEY_DIACRITICS},
     {NULL, 0, 0},
     {"__", 0, DIALOG__KEY_SPACE},
     {NULL, 0, 0},
     {"<", 0, DIALOG__KEY_CURSOR_LEFT},
     {">", 0, DIALOG__KEY_CURSOR_RIGHT},
     {"<-", 0, DIALOG__KEY_DELETE},
     {"X", 0, DIALOG__KEY_CANCEL}},
};

static const char *s_diacritic_upper[4][12] = {
    {"Á", "À", "Â", "Ã", "Ä", "Å", "Ç", "Ę", "É", "È", "Ê", "Ë"},
    {"Í", "Ì", "Î", "Ï", "Ñ", "Ó", "Ò", "Ô", "Õ", "Ö", "Ú", "Ù"},
    {"Û", "Ü", "Ý", "Ś", "ẞ", "Ø", "Ł", "Ń", "Ź", "Ż", "Đ", "Ň"},
    {"Ă", "Ą", "Ć", "Č", "Ď", "Ğ", "I", "Ŕ", "Ě", "Ţ", "Ř", "€"},
};

static const dialog__key_t s_hex_keys[5][5] = {
    {{"0", '0', 0},             {"1", '1', 0}, {"2", '2', 0}, {"3", '3', 0}, {NULL, 0, 0}},
    {{"4", '4', 0},             {"5", '5', 0}, {"6", '6', 0}, {"7", '7', 0}, {NULL, 0, 0}},
    {{"8", '8', 0},             {"9", '9', 0}, {"A", 'A', 0}, {"B", 'B', 0}, {NULL, 0, 0}},
    {{"C", 'C', 0},             {"D", 'D', 0}, {"E", 'E', 0}, {"F", 'F', 0}, {NULL, 0, 0}},
    {{"OK", 0, DIALOG__KEY_OK},
     {"DEL", 0, DIALOG__KEY_DELETE},
     {"CAN", 0, DIALOG__KEY_CANCEL},
     {"<", 0, DIALOG__KEY_CURSOR_LEFT},
     {">", 0, DIALOG__KEY_CURSOR_RIGHT}                                                  },
};

static const dialog__key_t s_num_keys[5][5] = {
    {{"1", '1', 0},             {"2", '2', 0}, {"3", '3', 0}, {NULL, 0, 0}, {NULL, 0, 0}},
    {{"4", '4', 0},             {"5", '5', 0}, {"6", '6', 0}, {NULL, 0, 0}, {NULL, 0, 0}},
    {{"7", '7', 0},             {"8", '8', 0}, {"9", '9', 0}, {NULL, 0, 0}, {NULL, 0, 0}},
    {{"-", '-', 0},             {"0", '0', 0}, {".", '.', 0}, {NULL, 0, 0}, {NULL, 0, 0}},
    {{"OK", 0, DIALOG__KEY_OK},
     {"DEL", 0, DIALOG__KEY_DELETE},
     {"CAN", 0, DIALOG__KEY_CANCEL},
     {"<", 0, DIALOG__KEY_CURSOR_LEFT},
     {">", 0, DIALOG__KEY_CURSOR_RIGHT}                                                 },
};

typedef struct {
    const dialog__key_t *keys;
    int rows, cols, sel_row, sel_col;
    bool caps, diacritics, mask_input;
    char *buffer;
    size_t buffer_size, len, max_len, cursor;
    const char *title, *prompt;
} dialog__keyboard_state_t;

static const char *
dialog__diacritic_label(const dialog__keyboard_state_t *st, int row, int col, const char *label) {
    if (st->diacritics && st->caps && row < 4) return s_diacritic_upper[row][col];
    return label;
}

static int dialog__utf8_length(const char *text) {
    int length = 0;
    for (; *text != '\0'; ++text)
        if (((unsigned char)*text & 0xc0) != 0x80) ++length;
    return length;
}

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

static size_t dialog__utf8_previous(const char *text, size_t offset) {
    if (offset == 0) return 0;
    do { offset--; } while (offset > 0 && ((unsigned char)text[offset] & 0xc0) == 0x80);
    return offset;
}

static size_t dialog__utf8_next(const char *text, size_t length, size_t offset) {
    if (offset >= length) return length;
    offset++;
    while (offset < length && ((unsigned char)text[offset] & 0xc0) == 0x80) offset++;
    return offset;
}

static void dialog__key_add(dialog__keyboard_state_t *st, char c) {
    if (st->len + 1 >= st->buffer_size || st->len >= st->max_len) return;
    memmove(st->buffer + st->cursor + 1, st->buffer + st->cursor, st->len - st->cursor + 1);
    st->buffer[st->cursor++] = c;
    st->len++;
    st->buffer[st->len] = '\0';
}

static void dialog__key_add_string(dialog__keyboard_state_t *st, const char *text) {
    size_t text_len = strlen(text);
    if (text_len == 0 || st->len + text_len >= st->buffer_size || st->len + text_len > st->max_len) return;
    memmove(st->buffer + st->cursor + text_len, st->buffer + st->cursor, st->len - st->cursor + 1);
    memcpy(st->buffer + st->cursor, text, text_len);
    st->cursor += text_len;
    st->len += text_len;
}

static void dialog__key_delete_before_cursor(dialog__keyboard_state_t *st) {
    if (st->cursor == 0) return;
    size_t previous = dialog__utf8_previous(st->buffer, st->cursor);
    memmove(st->buffer + previous, st->buffer + st->cursor, st->len - st->cursor + 1);
    st->len -= st->cursor - previous;
    st->cursor = previous;
}

static void dialog__key_move_cursor(dialog__keyboard_state_t *st, int direction) {
    if (direction < 0) st->cursor = dialog__utf8_previous(st->buffer, st->cursor);
    else st->cursor = dialog__utf8_next(st->buffer, st->len, st->cursor);
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

static uint16_t dialog__input_primary_color(void) {
    uint16_t pri, sec, bg, surface, text, muted, border, success, warning, error;
    config__get_colors_internal(
        &pri, &sec, &bg, &surface, &text, &muted, &border, &success, &warning, &error
    );
    return pri;
}

static void
dialog__keyboard_layout(const dialog__keyboard_state_t *st, int *text_area_h, int *cell_w, int *cell_h) {
    int w = display__width(), h = display__height();
    *text_area_h = st->cols == 12 ? DIALOG__CHAR_H * 3 + 4 : DIALOG__CHAR_H * 5 + 12;
    *cell_w = w / st->cols;
    *cell_h = (h - *text_area_h) / st->rows;
    if (*cell_w < 1) *cell_w = 1;
    if (*cell_h < 1) *cell_h = 1;
}

static char
dialog__text_key_code(const dialog__keyboard_state_t *st, int row, int col, const dialog__key_t *key) {
    static const char symbols[] = "!@#$%^&*()_+"
                                  "QWERTYUIOP{}"
                                  "ASDFGHJKL:'\\"
                                  "|ZXCVBNM<>~`";
    if (st->caps && !st->diacritics && !key->special) return symbols[row * 12 + col];
    if (st->caps && !st->diacritics && isalpha((unsigned char)key->code))
        return (char)toupper((unsigned char)key->code);
    return key->code;
}

static void dialog__keyboard_draw_cell(
    const dialog__keyboard_state_t *st, dialog__input_kind_t kind, int row, int col, int text_area_h,
    int cell_w, int cell_h, uint16_t bg
) {
    const dialog__key_t *key = &st->keys[row * st->cols + col];
    if (!dialog__key_valid(key)) return;
    int width = 1;
    if (st->cols == 12 && row == 4) {
        if (col == 0 || col == 2 || col == 4 || col == 6) width = 2;
    }
    int x = col * cell_w, y = text_area_h + row * cell_h;
    int key_w = width * cell_w;
    bool selected = row == st->sel_row && col == st->sel_col;
    const char *label = key->label;
    char transformed[2];
    if (!key->special && kind == DIALOG__INPUT_TEXT && !st->diacritics) {
        transformed[0] = dialog__text_key_code(st, row, col, key);
        transformed[1] = '\0';
        label = transformed;
    } else if (!key->special && st->diacritics) {
        label = dialog__diacritic_label(st, row, col, label);
    }
    display__fill_rect(x, y, key_w, cell_h, selected ? BRUCE_COLOR_WHITE : bg);
    if (selected) {
        display__set_text_color(BRUCE_COLOR_BLACK);
    } else {
        if ((st->cols == 12 && row >= 4) || key->special != 0) {
            display__draw_rect(x, y, key_w, cell_h, BRUCE_COLOR_WHITE);
        }
        display__set_text_color(BRUCE_COLOR_WHITE);
    }
    int text_size = st->cols == 12 ? 2 : DIALOG__TEXT_SIZE;
    if (st->cols == 12 && row == 4 && col == 10) text_size = DIALOG__TEXT_SIZE;
    int label_x = x + (key_w - dialog__utf8_length(label) * DIALOG__CHAR_W * text_size) / 2;
    int label_y = y + (cell_h - DIALOG__CHAR_H * text_size) / 2;
    if (label_x < x + 1) label_x = x + 1;
    if (label_y < y + 1) label_y = y + 1;
    if (row != st->rows - 1) label_y -= 2;
    else if (st->cols == 12 && col != 10) label_y -= 2;
    display__set_text_size(text_size);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_cursor(label_x, label_y);
    display__print(label);
}

static bruce_result_t dialog__keyboard_draw(dialog__keyboard_state_t *st, dialog__input_kind_t kind) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    int w = display__width(), text_area_h, cell_w, cell_h;
    dialog__keyboard_layout(st, &text_area_h, &cell_w, &cell_h);
    if (st->cols == 12) {
        (void)display__fill_screen(dialog__input_background_color());
    } else {
        dialog__input_clear_and_title(st->title);
    }
    display__set_text_color(BRUCE_COLOR_WHITE);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    int prompt_y = st->cols == 12 ? DIALOG__MARGIN : DIALOG__CHAR_H + 8;
    display__set_cursor(DIALOG__MARGIN, prompt_y);
    display__print(st->prompt != NULL ? st->prompt : (st->title != NULL ? st->title : ""));
    char counter[32];
    snprintf(counter, sizeof(counter), "%zu/%zu", st->len, st->max_len);
    display__set_cursor(w - (int)strlen(counter) * DIALOG__CHAR_W - DIALOG__MARGIN, prompt_y);
    display__print(counter);
    int textbox_y = st->cols == 12 ? DIALOG__CHAR_H + 4 : DIALOG__CHAR_H * 2 + 10;
    int textbox_h = st->cols == 12 ? DIALOG__CHAR_H * 2 : DIALOG__CHAR_H * 2 + 4;
    display__fill_rect(
        DIALOG__MARGIN, textbox_y, w - 2 * DIALOG__MARGIN, textbox_h, dialog__input_background_color()
    );
    display__draw_rect(
        DIALOG__MARGIN, textbox_y, w - 2 * DIALOG__MARGIN, textbox_h, dialog__input_primary_color()
    );
    int input_text_size = st->cols == 12 ? 2 : DIALOG__TEXT_SIZE;
    display__set_text_size(input_text_size);
    int max_chars = (w - 4 * DIALOG__MARGIN) / (DIALOG__CHAR_W * input_text_size);
    if (max_chars < 1) max_chars = 1;
    size_t view_start = 0;
    int chars_before_cursor = 0;
    for (size_t offset = 0; offset < st->cursor; offset = dialog__utf8_next(st->buffer, st->len, offset)) {
        chars_before_cursor++;
    }
    while (chars_before_cursor >= max_chars) {
        view_start = dialog__utf8_next(st->buffer, st->len, view_start);
        chars_before_cursor--;
    }

    int input_x = DIALOG__MARGIN + 2;
    int input_y = textbox_y - 1;
    int cursor_column = chars_before_cursor;
    int drawn_chars = 0;
    for (size_t offset = view_start; offset < st->len && drawn_chars < max_chars;) {
        if (offset == st->cursor) cursor_column = drawn_chars;
        size_t next = dialog__utf8_next(st->buffer, st->len, offset);
        if (st->mask_input) {
            display__set_cursor(input_x + drawn_chars * DIALOG__CHAR_W * input_text_size, input_y);
            display__print("*");
        } else {
            char glyph[5] = {0};
            size_t glyph_len = next - offset;
            if (glyph_len >= sizeof(glyph)) glyph_len = sizeof(glyph) - 1;
            memcpy(glyph, st->buffer + offset, glyph_len);
            display__set_cursor(input_x + drawn_chars * DIALOG__CHAR_W * input_text_size, input_y);
            display__print(glyph);
        }
        offset = next;
        drawn_chars++;
    }
    if (st->cursor >= st->len) cursor_column = drawn_chars;
    display__fill_rect(
        input_x + cursor_column * DIALOG__CHAR_W * input_text_size,
        input_y + 2,
        1,
        DIALOG__CHAR_H * input_text_size - 2,
        BRUCE_COLOR_WHITE
    );
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
        cols = 12;
    } else if (kind == DIALOG__INPUT_HEX) {
        keys = &s_hex_keys[0][0];
        rows = 5;
        cols = 5;
    } else {
        keys = &s_num_keys[0][0];
        rows = 5;
        cols = 5;
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
    st.cursor = st.len;
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
        if (ev.type == BRUCE_INPUT_KEY && ev.code == '\n') {
            const dialog__key_t *key = dialog__key_current(&st);
            if (dialog__key_valid(key) && key->special == DIALOG__KEY_CURSOR_LEFT) {
                dialog__key_move_cursor(&st, -1);
                full_redraw = true;
                continue;
            }
            if (dialog__key_valid(key) && key->special == DIALOG__KEY_CURSOR_RIGHT) {
                dialog__key_move_cursor(&st, 1);
                full_redraw = true;
                continue;
            }
            return BRUCE_OK;
        }

        /* Cardputer Fn arrows share their codes with ; . , and /. */
        bool printable_navigation_code =
            ev.type == BRUCE_INPUT_KEY && ev.value == ev.code &&
            (ev.code == BRUCE_INPUT_CODE_UP || ev.code == BRUCE_INPUT_CODE_DOWN ||
             ev.code == BRUCE_INPUT_CODE_LEFT || ev.code == BRUCE_INPUT_CODE_RIGHT);
        if (printable_navigation_code) {
            if (kind == DIALOG__INPUT_TEXT || dialog__key_validate(&st, (char)ev.code, kind)) {
                dialog__key_add(&st, (char)ev.code);
                full_redraw = true;
            }
            continue;
        }

        switch (ev.code) {
            case BRUCE_INPUT_CODE_UP:
            case BRUCE_INPUT_CODE_DOWN:
            case BRUCE_INPUT_CODE_PREV:
            case BRUCE_INPUT_CODE_NEXT:
            case BRUCE_INPUT_CODE_LEFT:
            case BRUCE_INPUT_CODE_RIGHT: {
                int previous_row = st.sel_row, previous_col = st.sel_col;
                if (ev.code == BRUCE_INPUT_CODE_UP) dialog__key_move_vertical_or_horizontal(&st, -1, 0);
                else if (ev.code == BRUCE_INPUT_CODE_DOWN && st.cols == 12 && st.sel_row == st.rows - 2) {
                    /* The action row has spanning buttons. Map every character column
                     * to its nearest action instead of wrapping through a spacer cell. */
                    static const int action_columns[] = {0, 2, 4, 6, 8, 9, 10, 11};
                    int action = 0;
                    for (int i = 1; i < (int)(sizeof(action_columns) / sizeof(action_columns[0])); ++i) {
                        if (st.sel_col < action_columns[i]) break;
                        action = i;
                    }
                    st.sel_row = st.rows - 1;
                    st.sel_col = action_columns[action];
                } else if (ev.code == BRUCE_INPUT_CODE_DOWN)
                    dialog__key_move_vertical_or_horizontal(&st, 1, 0);
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
                    dialog__key_delete_before_cursor(&st);
                } else if (key->special == DIALOG__KEY_SPACE) {
                    if (kind == DIALOG__INPUT_TEXT) dialog__key_add(&st, ' ');
                } else if (key->special == DIALOG__KEY_CAPS) st.caps = !st.caps;
                else if (key->special == DIALOG__KEY_DIACRITICS) {
                    st.diacritics = !st.diacritics;
                    st.caps = false;
                    st.keys = st.diacritics ? &s_diacritic_keys[0][0] : &s_text_keys[0][0];
                } else if (key->special == DIALOG__KEY_CURSOR_LEFT) {
                    dialog__key_move_cursor(&st, -1);
                } else if (key->special == DIALOG__KEY_CURSOR_RIGHT) {
                    dialog__key_move_cursor(&st, 1);
                } else if (st.diacritics) {
                    dialog__key_add_string(
                        &st, dialog__diacritic_label(&st, st.sel_row, st.sel_col, key->label)
                    );
                } else if (key->code != '\0') {
                    char c = kind == DIALOG__INPUT_TEXT
                                 ? dialog__text_key_code(&st, st.sel_row, st.sel_col, key)
                                 : key->code;
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
                dialog__key_delete_before_cursor(&st);
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
