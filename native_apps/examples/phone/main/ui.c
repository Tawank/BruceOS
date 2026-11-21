#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/clock.h"
#include "core_sdk/display.h"
#include "core_sdk/icon.h"

#define UI_GAP 4
#define UI_TAB_ICON 16

typedef struct {
    bruce_display_color_t bg, surface, outline, text, muted, accent, danger, on_accent, highlight;
} ui_palette_t;

static const ui_palette_t s_lcd = {
    .bg = 0x0841,
    .surface = 0x2124,
    .outline = 0x3186,
    .text = BRUCE_COLOR_WHITE,
    .muted = 0xAD55,
    .accent = 0x362B,
    .danger = 0xF9C6,
    .on_accent = BRUCE_COLOR_WHITE,
    .highlight = 0x31A6,
};

/* Black ink on white paper; filled shapes carry white text. */
static const ui_palette_t s_paper = {
    .bg = BRUCE_COLOR_WHITE,
    .surface = BRUCE_COLOR_WHITE,
    .outline = BRUCE_COLOR_BLACK,
    .text = BRUCE_COLOR_BLACK,
    .muted = BRUCE_COLOR_BLACK,
    .accent = BRUCE_COLOR_BLACK,
    .danger = BRUCE_COLOR_BLACK,
    .on_accent = BRUCE_COLOR_WHITE,
    .highlight = BRUCE_COLOR_BLACK,
};

static int16_t s_cw = 6;
static int16_t s_ch = 8;

static const char *const s_tab_names[UI_TAB_COUNT] = {"Keypad", "Recents", "Contacts"};
static const char *const s_tab_icons[UI_TAB_COUNT] = {"dialpad", "history", "account"};
static const char *const s_key_letters[UI_KEY_COUNT] = {
    "",
    "ABC",
    "DEF",
    "GHI",
    "JKL",
    "MNO",
    "PQRS",
    "TUV",
    "WXYZ",
    "",
    "+",
    "",
};

void ui__init_metrics(void) {
    display__set_text_size(1);
    int16_t w = 0, h = 0;
    if (display__get_font_metrics(&w, &h) == BRUCE_OK && w > 0 && h > 0) {
        s_cw = w;
        s_ch = h;
    }
}

static const ui_palette_t *ui__palette(const ui_t *ui) { return ui->epaper ? &s_paper : &s_lcd; }

bool ui__in_call(const ui_t *ui) { return ui->status.call != BRUCE_CALL_NONE; }

char ui__key_char(int index) {
    static const char keys[UI_KEY_COUNT] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#'};
    return index >= 0 && index < UI_KEY_COUNT ? keys[index] : 0;
}

/* ---- Layout ---- */

static int ui__bar_height(void) { return s_ch + 8; }

static int ui__tabs_height(void) { return UI_TAB_ICON + s_ch + 8; }

static int ui__content_bottom(int height) { return height - ui__tabs_height(); }

static int ui__display_height(void) { return 3 * s_ch + 12; }

static int ui__keys_top(void) { return ui__bar_height() + ui__display_height(); }

static int ui__key_row_height(int height) { return (ui__content_bottom(height) - ui__keys_top()) / 5; }

static int ui__key_width(int width) { return (width - 4 * UI_GAP) / 3; }

static int ui__list_row_height(void) { return 2 * s_ch + 12; }

ui_rect_t ui__tab_rect(int tab, int width, int height) {
    int w = width / UI_TAB_COUNT;
    return (ui_rect_t){
        tab * w, ui__content_bottom(height), tab == UI_TAB_COUNT - 1 ? width - tab * w : w, ui__tabs_height()
    };
}

ui_rect_t ui__key_rect(int index, int width, int height) {
    int row_h = ui__key_row_height(height);
    int key_w = ui__key_width(width);
    int row = index / 3, col = index % 3;
    return (ui_rect_t){
        UI_GAP + col * (key_w + UI_GAP), ui__keys_top() + row * row_h + UI_GAP / 2, key_w, row_h - UI_GAP
    };
}

ui_rect_t ui__call_button_rect(int width, int height) {
    int row_h = ui__key_row_height(height);
    int key_w = ui__key_width(width);
    return (ui_rect_t){(width - key_w) / 2, ui__keys_top() + 4 * row_h + UI_GAP / 2, key_w, row_h - UI_GAP};
}

ui_rect_t ui__save_number_rect(int width, int height) {
    ui_rect_t call = ui__call_button_rect(width, height);
    int size = call.h;
    int key_w = ui__key_width(width);
    return (ui_rect_t){UI_GAP + (key_w - size) / 2, call.y, size, size};
}

ui_rect_t ui__backspace_rect(int width, int height) {
    (void)height;
    int size = 2 * s_ch + 8;
    return (ui_rect_t){width - UI_GAP - size, ui__bar_height() + 2, size, size};
}

int ui__visible_rows(int height) {
    int rows = (ui__content_bottom(height) - ui__bar_height()) / ui__list_row_height();
    return rows > 0 ? rows : 1;
}

ui_rect_t ui__row_rect(int visible_index, int width, int height) {
    (void)height;
    int h = ui__list_row_height();
    return (ui_rect_t){0, ui__bar_height() + visible_index * h, width, h};
}

static int ui__min(int a, int b) { return a < b ? a : b; }

static int ui__button_radius(int width) { return ui__min(width / 10, 22); }

static int ui__end_radius(int width) { return ui__min(width / 8, 28); }

static int ui__buttons_y(int height) {
    int top = ui__bar_height();
    return top + (height - top) * 64 / 100;
}

static ui_rect_t ui__circle(int cx, int cy, int r) { return (ui_rect_t){cx - r, cy - r, 2 * r, 2 * r}; }

ui_rect_t ui__call_screen_rect(ui_hit_kind_t button, bool incoming, int width, int height) {
    int r = ui__button_radius(width);
    int end = ui__end_radius(width);
    int end_y = height - end - 10;
    switch (button) {
        case UI_HIT_MUTE: return ui__circle(width / 4, ui__buttons_y(height), r);
        case UI_HIT_DTMF_PAD: return ui__circle(width / 2, ui__buttons_y(height), r);
        case UI_HIT_VOLUME: return ui__circle(3 * width / 4, ui__buttons_y(height), r);
        case UI_HIT_DECLINE: return ui__circle(width / 4, end_y, end);
        case UI_HIT_ANSWER: return ui__circle(3 * width / 4, end_y, end);
        case UI_HIT_HANG_UP: return ui__circle(incoming ? width / 4 : width / 2, end_y, end);
        default: return (ui_rect_t){0, 0, 0, 0};
    }
}

ui_rect_t ui__dtmf_key_rect(int index, int width, int height) {
    int top = ui__bar_height() + 2 * s_ch + 12;
    int bottom = ui__buttons_y(height) - ui__button_radius(width) - 6;
    int row_h = (bottom - top) / 4;
    int key_w = ui__key_width(width);
    int row = index / 3, col = index % 3;
    return (ui_rect_t){
        UI_GAP + col * (key_w + UI_GAP), top + row * row_h + UI_GAP / 2, key_w, row_h - UI_GAP
    };
}

size_t ui__row_count(const ui_t *ui) {
    if (ui->tab == UI_TAB_RECENTS) return ui->book->call_count;
    if (ui->tab == UI_TAB_CONTACTS) return ui->book->contact_count + 1;
    return 0;
}

void ui__scroll_to_selection(ui_t *ui, int height) {
    int rows = ui__visible_rows(height);
    if (ui->selected < ui->scroll) ui->scroll = ui->selected;
    if (ui->selected >= ui->scroll + rows) ui->scroll = ui->selected - rows + 1;
    if (ui->scroll < 0) ui->scroll = 0;
}

static bool ui__inside(ui_rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static bool ui__can_save(const ui_t *ui) {
    return ui->number[0] != '\0' && !book__is_ussd(ui->number) &&
           book__name_for(ui->book, ui->number) == NULL;
}

ui_hit_t ui__hit(const ui_t *ui, int width, int height, int x, int y) {
    if (ui__in_call(ui)) {
        bool incoming = ui->status.call == BRUCE_CALL_INCOMING;
        if (incoming) {
            if (ui__inside(ui__call_screen_rect(UI_HIT_ANSWER, true, width, height), x, y))
                return (ui_hit_t){UI_HIT_ANSWER, 0};
            if (ui__inside(ui__call_screen_rect(UI_HIT_DECLINE, true, width, height), x, y))
                return (ui_hit_t){UI_HIT_DECLINE, 0};
            return (ui_hit_t){UI_HIT_NONE, 0};
        }
        static const ui_hit_kind_t buttons[] = {UI_HIT_MUTE, UI_HIT_DTMF_PAD, UI_HIT_VOLUME, UI_HIT_HANG_UP};
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
            if (ui__inside(ui__call_screen_rect(buttons[i], false, width, height), x, y))
                return (ui_hit_t){buttons[i], 0};
        }
        for (int i = 0; ui->dtmf_pad && i < UI_KEY_COUNT; ++i) {
            if (ui__inside(ui__dtmf_key_rect(i, width, height), x, y)) return (ui_hit_t){UI_HIT_KEY, i};
        }
        return (ui_hit_t){UI_HIT_NONE, 0};
    }
    for (int i = 0; i < UI_TAB_COUNT; ++i) {
        if (ui__inside(ui__tab_rect(i, width, height), x, y)) return (ui_hit_t){UI_HIT_TAB, i};
    }
    if (ui->tab == UI_TAB_KEYPAD) {
        for (int i = 0; i < UI_KEY_COUNT; ++i) {
            if (ui__inside(ui__key_rect(i, width, height), x, y)) return (ui_hit_t){UI_HIT_KEY, i};
        }
        if (ui__inside(ui__call_button_rect(width, height), x, y)) return (ui_hit_t){UI_HIT_CALL, 0};
        if (ui->number[0] != '\0' && ui__inside(ui__backspace_rect(width, height), x, y)) {
            return (ui_hit_t){UI_HIT_BACKSPACE, 0};
        }
        if (ui__can_save(ui) && ui__inside(ui__save_number_rect(width, height), x, y)) {
            return (ui_hit_t){UI_HIT_SAVE_NUMBER, 0};
        }
        return (ui_hit_t){UI_HIT_NONE, 0};
    }
    int rows = ui__visible_rows(height);
    for (int i = 0; i < rows; ++i) {
        int index = ui->scroll + i;
        if (index < (int)ui__row_count(ui) && ui__inside(ui__row_rect(i, width, height), x, y)) {
            return (ui_hit_t){UI_HIT_ROW, index};
        }
    }
    return (ui_hit_t){UI_HIT_NONE, 0};
}

/* ---- Text ---- */

void ui__format_duration(uint32_t seconds, char *out, size_t capacity) {
    if (seconds >= 3600) {
        snprintf(
            out,
            capacity,
            "%u:%02u:%02u",
            (unsigned)(seconds / 3600),
            (unsigned)(seconds / 60 % 60),
            (unsigned)(seconds % 60)
        );
    } else {
        snprintf(out, capacity, "%u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    }
}

static int64_t ui__day(int64_t local) { return local >= 0 ? local / 86400 : (local - 86399) / 86400; }

void ui__format_when(int64_t utc, int64_t now_utc, int64_t offset, char *out, size_t capacity) {
    static const char *const days[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (utc <= 0) {
        snprintf(out, capacity, "%s", "");
        return;
    }
    int64_t local = utc + offset;
    int64_t day = ui__day(local), today = ui__day(now_utc + offset);
    int64_t seconds = local - day * 86400;
    if (now_utc > 0 && day == today) {
        snprintf(out, capacity, "%02d:%02d", (int)(seconds / 3600), (int)(seconds / 60 % 60));
    } else if (now_utc > 0 && day == today - 1) {
        snprintf(out, capacity, "Yesterday");
    } else if (now_utc > 0 && day > today - 7 && day < today) {
        snprintf(out, capacity, "%s", days[(day % 7 + 11) % 7]);
    } else {
        bruce_clock_datetime_t date = {0};
        if (clock__epoch_to_datetime(local, &date) == BRUCE_OK) {
            snprintf(out, capacity, "%02u.%02u", date.day, date.month);
        } else {
            snprintf(out, capacity, "%s", "");
        }
    }
}

const char *ui__state_text(bruce_cellular_state_t state) {
    switch (state) {
        case BRUCE_CELLULAR_OFF: return "Phone off";
        case BRUCE_CELLULAR_STARTING: return "Starting";
        case BRUCE_CELLULAR_FAILED: return "Modem not answering";
        case BRUCE_CELLULAR_NO_SIM: return "No SIM card";
        case BRUCE_CELLULAR_SIM_LOCKED: return "SIM locked";
        case BRUCE_CELLULAR_SEARCHING: return "Searching";
        case BRUCE_CELLULAR_DENIED: return "Network denied";
        case BRUCE_CELLULAR_READY: return "Ready";
    }
    return "";
}

const char *ui__call_text(const bruce_cellular_status_t *status) {
    switch (status->call) {
        case BRUCE_CALL_DIALING: return "Calling";
        case BRUCE_CALL_ALERTING: return "Ringing";
        case BRUCE_CALL_INCOMING: return "Incoming call";
        default: return NULL;
    }
}

static const char *ui__result_text(const book_call_t *call) {
    switch (call->result) {
        case BRUCE_CALL_RESULT_MISSED: return "Missed";
        case BRUCE_CALL_RESULT_REJECTED: return "Declined";
        case BRUCE_CALL_RESULT_CANCELLED: return "Cancelled";
        case BRUCE_CALL_RESULT_BUSY: return "Busy";
        case BRUCE_CALL_RESULT_NO_ANSWER: return "No answer";
        case BRUCE_CALL_RESULT_FAILED: return "Failed";
        default: return call->incoming ? "Incoming" : "Outgoing";
    }
}

/* The last `max_chars` characters, so a long number keeps its end. */
static const char *ui__tail(const char *text, int max_chars) {
    int length = (int)strlen(text);
    return length > max_chars && max_chars > 0 ? text + length - max_chars : text;
}

static void ui__fit(const char *text, int max_chars, char *out, size_t capacity) {
    snprintf(out, capacity, "%s", text);
    if (max_chars >= 3 && (int)strlen(out) > max_chars) {
        out[max_chars - 2] = '.';
        out[max_chars - 1] = '.';
        out[max_chars] = '\0';
    }
}

/* ---- Drawing ---- */

static void ui__icon(const char *name, int x, int y, int size, bruce_display_color_t color) {
    const bruce_icon_t *icon = icon__get(name);
    if (icon != NULL)
        display__draw_bitmap_scaled(x, y, icon->bits, icon->width, icon->height, size, size, color);
}

static void ui__text(const char *text, int x, int y, float size, bruce_display_color_t color) {
    display__set_text_size(size);
    display__set_text_color(color);
    display__draw_string(text, x, y);
    display__set_text_size(1);
}

static void ui__centre_text(const char *text, int cx, int y, float size, bruce_display_color_t color) {
    display__set_text_size(size);
    display__set_text_color(color);
    display__draw_centre_string(text, cx, y);
    display__set_text_size(1);
}

static void ui__right_text(const char *text, int right, int y, bruce_display_color_t color) {
    display__set_text_color(color);
    display__draw_right_string(text, right, y);
}

/* A key: filled on LCDs, outlined on paper. */
static void ui__key_shape(const ui_t *ui, ui_rect_t r, int radius) {
    const ui_palette_t *p = ui__palette(ui);
    display__fill_round_rect(r.x, r.y, r.w, r.h, radius, p->surface);
    if (ui->epaper) display__draw_round_rect(r.x, r.y, r.w, r.h, radius, p->outline);
}

static void
ui__round_button(const ui_t *ui, ui_rect_t r, const char *icon, bool filled, bruce_display_color_t fill) {
    const ui_palette_t *p = ui__palette(ui);
    int radius = r.w / 2;
    int cx = r.x + radius, cy = r.y + radius;
    if (filled) {
        display__fill_circle(cx, cy, radius, fill);
    } else {
        display__fill_circle(cx, cy, radius, p->surface);
        if (ui->epaper) display__draw_circle(cx, cy, radius, p->outline);
    }
    int size = radius;
    ui__icon(icon, cx - size / 2, cy - size / 2, size, filled ? p->on_accent : p->text);
}

static void ui__draw_keys(const ui_t *ui, ui_rect_t (*rect)(int, int, int), int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    for (int i = 0; i < UI_KEY_COUNT; ++i) {
        ui_rect_t r = rect(i, width, height);
        ui__key_shape(ui, r, ui__min(r.h / 3, 12));
        char digit[2] = {ui__key_char(i), '\0'};
        bool letters = s_key_letters[i][0] != '\0' && r.h >= 3 * s_ch + 6;
        int digit_y = r.y + (r.h - 2 * s_ch - (letters ? s_ch + 2 : 0)) / 2;
        ui__centre_text(digit, r.x + r.w / 2, digit_y, 2, p->text);
        if (letters) ui__centre_text(s_key_letters[i], r.x + r.w / 2, digit_y + 2 * s_ch + 2, 1, p->muted);
    }
}

static void ui__draw_signal(const ui_t *ui, int width) {
    static const char *const icons[] = {
        "signal-cellular-outline",
        "signal-cellular-1",
        "signal-cellular-2",
        "signal-cellular-3",
        "signal-cellular-3",
    };
    int bars = ui->status.signal_bars;
    if (bars < 0 || bars > 4) return;
    int size = s_ch + 4;
    ui__icon(icons[bars], width - UI_GAP - size, 2, size, ui__palette(ui)->text);
}

static void ui__draw_bar(const ui_t *ui, int width) {
    const ui_palette_t *p = ui__palette(ui);
    const char *text = ui->status.state == BRUCE_CELLULAR_READY && ui->status.operator_name[0] != '\0'
                           ? ui->status.operator_name
                           : ui__state_text(ui->status.state);
    char fitted[BRUCE_CELLULAR_OPERATOR_MAX + 8];
    ui__fit(text, (width - 2 * s_ch - 16) / s_cw, fitted, sizeof(fitted));
    ui__text(fitted, UI_GAP, 4, 1, ui->status.state == BRUCE_CELLULAR_READY ? p->text : p->muted);
    ui__draw_signal(ui, width);
}

static void ui__draw_tabs(const ui_t *ui, int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    int top = ui__content_bottom(height);
    display__fill_rect(0, top, width, 1, p->outline);
    for (int i = 0; i < UI_TAB_COUNT; ++i) {
        ui_rect_t r = ui__tab_rect(i, width, height);
        bool selected = (int)ui->tab == i;
        bruce_display_color_t color = selected ? (ui->epaper ? p->on_accent : p->accent) : p->muted;
        if (selected && ui->epaper)
            display__fill_round_rect(r.x + 3, r.y + 3, r.w - 6, r.h - 5, 6, p->highlight);
        ui__icon(s_tab_icons[i], r.x + (r.w - UI_TAB_ICON) / 2, r.y + 3, UI_TAB_ICON, color);
        ui__centre_text(s_tab_names[i], r.x + r.w / 2, r.y + UI_TAB_ICON + 5, 1, color);
    }
}

static void ui__draw_keypad(const ui_t *ui, int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    int top = ui__bar_height();
    ui_rect_t back = ui__backspace_rect(width, height);
    if (ui->number[0] == '\0') {
        ui__centre_text("Enter a number", width / 2, top + s_ch / 2 + 4, 1, p->muted);
    } else {
        int max_chars = (back.x - 2 * UI_GAP) / (2 * s_cw);
        ui__centre_text(ui__tail(ui->number, max_chars), width / 2, top + 4, 2, p->text);
        ui__icon("backspace-outline", back.x + 2, back.y + 2, back.w - 4, p->text);
        const char *name = book__is_ussd(ui->number) ? "Service code" : book__name_for(ui->book, ui->number);
        if (name != NULL) {
            char fitted[BOOK_NAME_MAX];
            ui__fit(name, width / s_cw - 2, fitted, sizeof(fitted));
            ui__centre_text(fitted, width / 2, top + 2 * s_ch + 8, 1, p->muted);
        }
    }
    ui__draw_keys(ui, ui__key_rect, width, height);
    ui_rect_t call = ui__call_button_rect(width, height);
    display__fill_round_rect(call.x, call.y, call.w, call.h, call.h / 2, p->accent);
    int icon = ui__min(call.h - 8, 24);
    ui__icon("phone", call.x + (call.w - icon) / 2, call.y + (call.h - icon) / 2, icon, p->on_accent);
    if (ui__can_save(ui)) {
        ui_rect_t save = ui__save_number_rect(width, height);
        int size = ui__min(save.w - 8, 24);
        ui__icon("account-plus", save.x + (save.w - size) / 2, save.y + (save.h - size) / 2, size, p->text);
    }
}

static void ui__draw_row_frame(const ui_t *ui, ui_rect_t r, bool selected) {
    const ui_palette_t *p = ui__palette(ui);
    if (selected) display__fill_round_rect(r.x + 2, r.y + 1, r.w - 4, r.h - 2, 6, p->highlight);
    else display__fill_rect(r.x + 8, r.y + r.h - 1, r.w - 16, 1, ui->epaper ? p->outline : p->surface);
}

static void ui__draw_empty(const ui_t *ui, const char *text, int width, int height) {
    ui__centre_text(
        text,
        width / 2,
        (ui__bar_height() + ui__content_bottom(height)) / 2 - s_ch / 2,
        1,
        ui__palette(ui)->muted
    );
}

static void ui__draw_recents(const ui_t *ui, int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    if (ui->book->call_count == 0) {
        ui__draw_empty(ui, "No calls yet", width, height);
        return;
    }
    int rows = ui__visible_rows(height);
    for (int i = 0; i < rows && ui->scroll + i < (int)ui->book->call_count; ++i) {
        int index = ui->scroll + i;
        const book_call_t *call = &ui->book->calls[index];
        ui_rect_t r = ui__row_rect(i, width, height);
        bool selected = index == ui->selected;
        ui__draw_row_frame(ui, r, selected);
        bool missed = call->result == BRUCE_CALL_RESULT_MISSED;
        bruce_display_color_t text = selected && ui->epaper ? p->on_accent : p->text;
        bruce_display_color_t muted = selected && ui->epaper ? p->on_accent : p->muted;
        const char *icon = missed ? "phone-missed" : call->incoming ? "phone-incoming" : "phone-outgoing";
        ui__icon(icon, UI_GAP + 4, r.y + (r.h - 16) / 2, 16, missed && !ui->epaper ? p->danger : muted);

        char when[16];
        ui__format_when(call->started_utc, ui->now_utc, ui->utc_offset, when, sizeof(when));
        int text_x = UI_GAP + 28;
        int max_chars = (width - text_x - (int)strlen(when) * s_cw - 2 * UI_GAP) / s_cw;
        const char *name = book__name_for(ui->book, call->number);
        char line[BOOK_NAME_MAX + 8];
        ui__fit(
            name != NULL              ? name
            : call->number[0] != '\0' ? call->number
                                      : "Unknown",
            max_chars,
            line,
            sizeof(line)
        );
        ui__text(line, text_x, r.y + 5, 1, missed && !ui->epaper ? p->danger : text);
        ui__right_text(when, width - UI_GAP - 4, r.y + 5, muted);

        char detail[64], duration[16];
        ui__format_duration(call->seconds, duration, sizeof(duration));
        const char *result = call->result == BRUCE_CALL_RESULT_ANSWERED ? duration : ui__result_text(call);
        if (name != NULL) snprintf(detail, sizeof(detail), "%s  %s", result, call->number);
        else snprintf(detail, sizeof(detail), "%s", result);
        ui__fit(detail, (width - text_x - UI_GAP) / s_cw, line, sizeof(line));
        ui__text(line, text_x, r.y + s_ch + 9, 1, muted);
    }
}

static void ui__draw_avatar(const ui_t *ui, const char *name, int cx, int cy, int r, bool inverted) {
    const ui_palette_t *p = ui__palette(ui);
    bruce_display_color_t fill = inverted ? p->on_accent : ui->epaper ? p->text : p->highlight;
    display__fill_circle(cx, cy, r, fill);
    char initial[2] = {name != NULL && name[0] != '\0' && name[0] != '+' ? name[0] : 0, '\0'};
    bruce_display_color_t ink = inverted ? p->text : p->on_accent;
    if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = (char)(initial[0] - 'a' + 'A');
    if (initial[0] != '\0' && (initial[0] < '0' || initial[0] > '9')) {
        float size = r >= 2 * s_ch ? 2 : 1;
        ui__centre_text(initial, cx, cy - (int)(s_ch * size) / 2, size, ink);
    } else {
        ui__icon("account", cx - r * 2 / 3, cy - r * 2 / 3, r * 4 / 3, ink);
    }
}

static void ui__draw_contacts(const ui_t *ui, int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    int rows = ui__visible_rows(height);
    for (int i = 0; i < rows && ui->scroll + i < (int)ui__row_count(ui); ++i) {
        int index = ui->scroll + i;
        ui_rect_t r = ui__row_rect(i, width, height);
        bool selected = index == ui->selected;
        ui__draw_row_frame(ui, r, selected);
        bruce_display_color_t text = selected && ui->epaper ? p->on_accent : p->text;
        bruce_display_color_t muted = selected && ui->epaper ? p->on_accent : p->muted;
        int text_x = UI_GAP + 32;
        if (index == 0) {
            ui__icon(
                "account-plus",
                UI_GAP + 6,
                r.y + (r.h - 18) / 2,
                18,
                selected && ui->epaper ? p->on_accent : p->accent
            );
            ui__text("New contact", text_x, r.y + (r.h - s_ch) / 2, 1, text);
            continue;
        }
        const book_contact_t *contact = &ui->book->contacts[index - 1];
        ui__draw_avatar(ui, contact->name, UI_GAP + 14, r.y + r.h / 2, r.h / 2 - 4, selected && ui->epaper);
        char line[BOOK_NAME_MAX + 4];
        ui__fit(contact->name, (width - text_x - UI_GAP) / s_cw, line, sizeof(line));
        ui__text(line, text_x, r.y + 5, 1, text);
        ui__text(contact->number, text_x, r.y + s_ch + 9, 1, muted);
    }
}

static void ui__draw_call(const ui_t *ui, int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    const bruce_cellular_status_t *status = &ui->status;
    bool incoming = status->call == BRUCE_CALL_INCOMING;
    const char *name = book__name_for(ui->book, status->call_number);
    const char *number = status->call_number[0] != '\0' ? status->call_number : "Unknown";
    char line[BOOK_NAME_MAX + 8];
    char state[24];
    const char *call_text = ui__call_text(status);
    if (call_text != NULL) snprintf(state, sizeof(state), "%s", call_text);
    else ui__format_duration(status->call_seconds, state, sizeof(state));

    int top = ui__bar_height();
    if (ui->dtmf_pad && !incoming) {
        ui__fit(name != NULL ? name : number, width / s_cw - 2, line, sizeof(line));
        ui__centre_text(line, width / 2, top + 2, 1, p->muted);
        ui__centre_text(ui__tail(ui->dtmf, width / (2 * s_cw) - 1), width / 2, top + s_ch + 6, 2, p->text);
        ui__draw_keys(ui, ui__dtmf_key_rect, width, height);
    } else {
        int avail = height - top;
        int r = ui__min(width, avail) / 9;
        int cy = top + avail * 22 / 100;
        ui__draw_avatar(ui, name, width / 2, cy, r, false);
        int y = cy + r + 8;
        ui__fit(name != NULL ? name : number, width / (2 * s_cw) - 1, line, sizeof(line));
        ui__centre_text(line, width / 2, y, 2, p->text);
        y += 2 * s_ch + 4;
        if (name != NULL) {
            ui__centre_text(number, width / 2, y, 1, p->muted);
            y += s_ch + 4;
        }
        ui__centre_text(state, width / 2, y, 1, status->call == BRUCE_CALL_ACTIVE ? p->accent : p->muted);
    }

    if (incoming) {
        ui__round_button(
            ui, ui__call_screen_rect(UI_HIT_DECLINE, true, width, height), "phone-hangup", true, p->danger
        );
        ui__round_button(
            ui, ui__call_screen_rect(UI_HIT_ANSWER, true, width, height), "phone", true, p->accent
        );
        return;
    }
    ui__round_button(
        ui,
        ui__call_screen_rect(UI_HIT_MUTE, false, width, height),
        status->muted ? "microphone-off" : "microphone",
        status->muted,
        p->highlight
    );
    ui__round_button(
        ui, ui__call_screen_rect(UI_HIT_DTMF_PAD, false, width, height), "dialpad", ui->dtmf_pad, p->highlight
    );
    const char *volume = status->volume >= 70   ? "volume-high"
                         : status->volume >= 35 ? "volume-medium"
                                                : "volume-low";
    ui_rect_t volume_rect = ui__call_screen_rect(UI_HIT_VOLUME, false, width, height);
    ui__round_button(ui, volume_rect, volume, false, p->highlight);
    char level[8];
    snprintf(level, sizeof(level), "%u%%", (unsigned)status->volume);
    ui__centre_text(level, volume_rect.x + volume_rect.w / 2, volume_rect.y + volume_rect.h + 3, 1, p->muted);
    ui__centre_text(
        status->muted ? "Muted" : "Mute", width / 4, volume_rect.y + volume_rect.h + 3, 1, p->muted
    );
    ui__centre_text("Keypad", width / 2, volume_rect.y + volume_rect.h + 3, 1, p->muted);
    ui__round_button(
        ui, ui__call_screen_rect(UI_HIT_HANG_UP, false, width, height), "phone-hangup", true, p->danger
    );
}

void ui__draw(const ui_t *ui, int width, int height) {
    display__fill_screen(ui__palette(ui)->bg);
    display__set_text_size(1);
    ui__draw_bar(ui, width);
    if (ui__in_call(ui)) {
        ui__draw_call(ui, width, height);
        return;
    }
    switch (ui->tab) {
        case UI_TAB_KEYPAD: ui__draw_keypad(ui, width, height); break;
        case UI_TAB_RECENTS: ui__draw_recents(ui, width, height); break;
        case UI_TAB_CONTACTS: ui__draw_contacts(ui, width, height); break;
        default: break;
    }
    ui__draw_tabs(ui, width, height);
}
