#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/clock.h"
#include "core_sdk/display.h"
#include "core_sdk/icon.h"

#define UI_GAP 4
#define UI_BUBBLE_PAD 4
#define UI_LINES_MAX 40

typedef struct {
    bruce_display_color_t bg, surface, outline, text, muted, accent, on_accent, highlight;
} ui_palette_t;

/* The phone app's colours. */
static const ui_palette_t s_lcd = {
    .bg = 0x0841,
    .surface = 0x2124,
    .outline = 0x3186,
    .text = BRUCE_COLOR_WHITE,
    .muted = 0xAD55,
    .accent = 0x362B,
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
    .on_accent = BRUCE_COLOR_WHITE,
    .highlight = BRUCE_COLOR_BLACK,
};

static int16_t s_cw = 6;
static int16_t s_ch = 8;
static int s_origin_x, s_origin_y;

void ui__init_metrics(void) {
    display__set_text_size(1);
    int16_t w = 0, h = 0;
    if (display__get_font_metrics(&w, &h) == BRUCE_OK && w > 0 && h > 0) {
        s_cw = w;
        s_ch = h;
    }
}

void ui__set_origin(int x, int y) {
    s_origin_x = x;
    s_origin_y = y;
}

static const ui_palette_t *ui__palette(const ui_t *ui) { return ui->epaper ? &s_paper : &s_lcd; }

/* ---- Text ---- */

/* Bytes in the UTF-8 character starting at c. */
static size_t ui__char_bytes(const char *c) {
    unsigned char lead = (unsigned char)*c;
    size_t bytes = lead < 0x80             ? 1
                   : (lead & 0xE0) == 0xC0 ? 2
                   : (lead & 0xF0) == 0xE0 ? 3
                   : (lead & 0xF8) == 0xF0 ? 4
                                           : 1;
    for (size_t i = 1; i < bytes; ++i) {
        if (c[i] == '\0') return i;
    }
    return bytes;
}

static size_t ui__chars(const char *text, size_t bytes) {
    size_t count = 0;
    for (size_t i = 0; i < bytes; i += ui__char_bytes(text + i)) count++;
    return count;
}

size_t ui__wrap(const char *text, int max_chars, ui_line_t *lines, size_t capacity) {
    if (max_chars < 1) max_chars = 1;
    size_t count = 0;
    const char *c = text;
    while (*c != '\0' && count < capacity) {
        const char *start = c, *space = NULL;
        int chars = 0;
        while (*c != '\0' && *c != '\n' && chars < max_chars) {
            if (*c == ' ') space = c;
            c += ui__char_bytes(c);
            chars++;
        }
        /* Break at the last space when a word would be cut. */
        if (*c != '\0' && *c != '\n' && *c != ' ' && space != NULL) c = space;
        lines[count++] = (ui_line_t){start, (size_t)(c - start)};
        if (*c == '\n' || *c == ' ') c++;
    }
    return count;
}

static void ui__fit(const char *text, int max_chars, char *out, size_t capacity) {
    size_t at = 0;
    int chars = 0;
    while (text[at] != '\0' && text[at] != '\n' && chars < max_chars) {
        size_t bytes = ui__char_bytes(text + at);
        if (at + bytes >= capacity) break;
        at += bytes;
        chars++;
    }
    bool cut = text[at] != '\0';
    if (cut && chars >= 2 && max_chars >= 2) {
        /* Drop the last character for "..". */
        while (at > 0 && ((unsigned char)text[at - 1] & 0xC0) == 0x80) at--;
        if (at > 0) at--;
    }
    memcpy(out, text, at);
    out[at] = '\0';
    if (cut && max_chars >= 2 && at + 2 < capacity) strcat(out, "..");
}

static bool ui__ascii(const char *text) {
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        if (*c < 0x20 || *c > 0x7E) return false;
    }
    return true;
}

/* UCS2 characters; anything beyond the BMP takes two. */
static size_t ui__ucs2_chars(const char *text) {
    size_t count = 0;
    for (const char *c = text; *c != '\0'; c += ui__char_bytes(c)) count += ui__char_bytes(c) == 4 ? 2 : 1;
    return count;
}

void ui__draft_count(const char *draft, char *out, size_t capacity) {
    bool ascii = ui__ascii(draft);
    snprintf(
        out,
        capacity,
        "%u/%u",
        (unsigned)(ascii ? strlen(draft) : ui__ucs2_chars(draft)),
        (unsigned)(ascii ? BRUCE_CELLULAR_SMS_ASCII_MAX : BRUCE_CELLULAR_SMS_UCS2_MAX)
    );
}

bool ui__draft_fits(const char *draft) {
    return ui__ascii(draft) ? strlen(draft) <= BRUCE_CELLULAR_SMS_ASCII_MAX
                            : ui__ucs2_chars(draft) <= BRUCE_CELLULAR_SMS_UCS2_MAX;
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

/* ---- Layout ---- */

static int ui__bar_height(void) { return s_ch + 8; }

static int ui__row_height(void) { return 2 * s_ch + 12; }

static int ui__compose_height(void) { return 2 * s_ch + 12; }

int ui__thread_step(void) { return 2 * (s_ch + 2); }

int ui__visible_rows(int height) {
    int rows = (height - ui__bar_height()) / ui__row_height();
    return rows > 0 ? rows : 1;
}

ui_rect_t ui__row_rect(int visible_index, int width) {
    return (ui_rect_t){0, ui__bar_height() + visible_index * ui__row_height(), width, ui__row_height()};
}

ui_rect_t ui__send_rect(int width, int height) {
    int size = ui__compose_height() - 2 * UI_GAP;
    return (ui_rect_t){width - UI_GAP - size, height - ui__compose_height() + UI_GAP, size, size};
}

ui_rect_t ui__compose_rect(int width, int height) {
    ui_rect_t send = ui__send_rect(width, height);
    return (ui_rect_t){UI_GAP, send.y, send.x - 2 * UI_GAP, send.h};
}

void ui__scroll_to_selection(ui_t *ui, int height) {
    int rows = ui__visible_rows(height);
    if (ui->selected < ui->scroll) ui->scroll = ui->selected;
    if (ui->selected >= ui->scroll + rows) ui->scroll = ui->selected - rows + 1;
    if (ui->scroll < 0) ui->scroll = 0;
}

static int ui__bubble_chars(int width) { return (width * 3 / 4 - 2 * UI_BUBBLE_PAD) / s_cw; }

static int ui__line_height(void) { return s_ch + 2; }

/* A bubble with its time under it. */
static int ui__bubble_height(const store_message_t *message, int width) {
    ui_line_t lines[UI_LINES_MAX];
    size_t count = ui__wrap(message->text, ui__bubble_chars(width), lines, UI_LINES_MAX);
    if (count == 0) count = 1;
    return (int)count * ui__line_height() + 2 * UI_BUBBLE_PAD + s_ch + 2 * UI_GAP;
}

static int ui__thread_top(void) { return ui__bar_height(); }

static int ui__thread_bottom(int height) { return height - ui__compose_height(); }

int ui__thread_max_scroll(const ui_t *ui, int width, int height) {
    size_t indices[STORE_MESSAGES_MAX];
    size_t count = store__thread_messages(ui->store, ui->number, indices, STORE_MESSAGES_MAX);
    int total = 0;
    for (size_t i = 0; i < count; ++i) total += ui__bubble_height(&ui->store->messages[indices[i]], width);
    int room = ui__thread_bottom(height) - ui__thread_top();
    return total > room ? total - room : 0;
}

static bool ui__inside(ui_rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

ui_hit_t ui__hit(const ui_t *ui, int width, int height, int x, int y) {
    if (ui->screen == UI_SCREEN_THREAD) {
        if (y < ui__bar_height()) return (ui_hit_t){UI_HIT_BACK, 0};
        if (ui__inside(ui__send_rect(width, height), x, y)) return (ui_hit_t){UI_HIT_SEND, 0};
        if (ui__inside(ui__compose_rect(width, height), x, y)) return (ui_hit_t){UI_HIT_COMPOSE, 0};
        return (ui_hit_t){UI_HIT_NONE, 0};
    }
    int rows = ui__visible_rows(height);
    for (int i = 0; i < rows; ++i) {
        int index = ui->scroll + i;
        if (index <= (int)ui->thread_count && ui__inside(ui__row_rect(i, width), x, y))
            return (ui_hit_t){UI_HIT_ROW, index};
    }
    return (ui_hit_t){UI_HIT_NONE, 0};
}

/* ---- Drawing ---- */

static void ui__fill_rect(int x, int y, int w, int h, bruce_display_color_t color) {
    display__fill_rect(s_origin_x + x, s_origin_y + y, w, h, color);
}

static void ui__round_rect(ui_rect_t r, int radius, bruce_display_color_t color, bool fill) {
    if (fill) display__fill_round_rect(s_origin_x + r.x, s_origin_y + r.y, r.w, r.h, radius, color);
    else display__draw_round_rect(s_origin_x + r.x, s_origin_y + r.y, r.w, r.h, radius, color);
}

static void ui__disc(int cx, int cy, int radius, bruce_display_color_t color) {
    display__fill_circle(s_origin_x + cx, s_origin_y + cy, radius, color);
}

static void ui__icon(const char *name, int x, int y, int size, bruce_display_color_t color) {
    const bruce_icon_t *icon = icon__get(name);
    if (icon != NULL) {
        display__draw_bitmap_scaled(
            s_origin_x + x, s_origin_y + y, icon->bits, icon->width, icon->height, size, size, color
        );
    }
}

static void ui__text(const char *text, int x, int y, bruce_display_color_t color) {
    display__set_text_color(color);
    display__draw_string(text, s_origin_x + x, s_origin_y + y);
}

static void ui__right_text(const char *text, int right, int y, bruce_display_color_t color) {
    display__set_text_color(color);
    display__draw_right_string(text, s_origin_x + right, s_origin_y + y);
}

static void ui__centre_text(const char *text, int cx, int y, bruce_display_color_t color) {
    display__set_text_color(color);
    display__draw_centre_string(text, s_origin_x + cx, s_origin_y + y);
}

static const char *ui__display_name(const ui_t *ui, const char *number) {
    const char *name = store__name_for(ui->store, number);
    return name != NULL ? name : number;
}

static void ui__draw_avatar(const ui_t *ui, const char *name, int cx, int cy, int r) {
    const ui_palette_t *p = ui__palette(ui);
    ui__disc(cx, cy, r, ui->epaper ? p->text : p->highlight);
    char initial[2] = {name[0], '\0'};
    if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = (char)(initial[0] - 'a' + 'A');
    if (initial[0] >= 'A' && initial[0] <= 'Z') ui__centre_text(initial, cx, cy - s_ch / 2, p->on_accent);
    else ui__icon("account", cx - r * 2 / 3, cy - r * 2 / 3, r * 4 / 3, p->on_accent);
}

static void ui__draw_list(const ui_t *ui, int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    ui__text("Messages", UI_GAP, 4, p->text);
    if (ui->status.state != BRUCE_CELLULAR_READY)
        ui__right_text(ui__state_text(ui->status.state), width - UI_GAP, 4, p->muted);
    int rows = ui__visible_rows(height);
    int r = s_ch + 2;
    int text_x = UI_GAP + 2 * r + UI_GAP * 2;
    for (int i = 0; i < rows; ++i) {
        int index = ui->scroll + i;
        if (index > (int)ui->thread_count) break;
        ui_rect_t row = ui__row_rect(i, width);
        bool selected = index == ui->selected;
        if (selected)
            ui__round_rect((ui_rect_t){row.x + 2, row.y + 1, row.w - 4, row.h - 2}, 6, p->highlight, true);
        else ui__fill_rect(row.x + 8, row.y + row.h - 1, row.w - 16, 1, ui->epaper ? p->outline : p->surface);
        bruce_display_color_t text = selected && ui->epaper ? p->on_accent : p->text;
        bruce_display_color_t muted = selected && ui->epaper ? p->on_accent : p->muted;
        int cy = row.y + row.h / 2;
        if (index == 0) {
            ui__icon("message-plus", UI_GAP + 2, cy - r, 2 * r, text);
            ui__text("New message", text_x, cy - s_ch / 2, text);
            continue;
        }
        const store_thread_t *thread = &ui->threads[index - 1];
        const store_message_t *last = &ui->store->messages[thread->last];
        const char *name = ui__display_name(ui, last->number);
        ui__draw_avatar(ui, name, UI_GAP + r, cy, r);
        char when[16], line[64];
        ui__format_when(last->utc, ui->now_utc, ui->utc_offset, when, sizeof(when));
        int max_chars = (width - text_x - (int)strlen(when) * s_cw - 3 * UI_GAP) / s_cw;
        ui__fit(name, max_chars, line, sizeof(line));
        ui__text(line, text_x, row.y + 5, thread->unread > 0 && !selected ? p->accent : text);
        ui__right_text(when, width - UI_GAP - 4, row.y + 5, muted);
        char preview[BRUCE_CELLULAR_TEXT_MAX + 8];
        snprintf(preview, sizeof(preview), "%s%s", last->incoming ? "" : "You: ", last->text);
        int preview_chars = (width - text_x - UI_GAP - (thread->unread > 0 ? 3 * UI_GAP : 0)) / s_cw;
        ui__fit(preview, preview_chars, line, sizeof(line));
        ui__text(line, text_x, row.y + s_ch + 9, muted);
        if (thread->unread > 0)
            ui__disc(width - UI_GAP - 6, row.y + s_ch + 9 + s_ch / 2, 3, selected ? muted : p->accent);
    }
    if (ui->thread_count == 0 && rows > 1) {
        ui__centre_text("No messages yet", width / 2, ui__row_rect(1, width).y + s_ch, p->muted);
    }
}

static void ui__draw_bubble(const ui_t *ui, const store_message_t *message, int width, int bottom) {
    const ui_palette_t *p = ui__palette(ui);
    ui_line_t lines[UI_LINES_MAX];
    size_t count = ui__wrap(message->text, ui__bubble_chars(width), lines, UI_LINES_MAX);
    int widest = 1;
    for (size_t i = 0; i < count; ++i) {
        int chars = (int)ui__chars(lines[i].start, lines[i].bytes);
        if (chars > widest) widest = chars;
    }
    if (count == 0) lines[count++] = (ui_line_t){"", 0};
    int w = widest * s_cw + 2 * UI_BUBBLE_PAD;
    int h = (int)count * ui__line_height() + 2 * UI_BUBBLE_PAD;
    int time_y = bottom - UI_GAP - s_ch;
    ui_rect_t r = {message->incoming ? UI_GAP : width - UI_GAP - w, time_y - UI_GAP / 2 - h, w, h};
    bruce_display_color_t ink = message->incoming ? p->text : p->on_accent;
    ui__round_rect(r, 6, message->incoming ? p->surface : p->accent, true);
    if (ui->epaper && message->incoming) ui__round_rect(r, 6, p->outline, false);
    for (size_t i = 0; i < count; ++i) {
        char text[BRUCE_CELLULAR_TEXT_MAX];
        size_t bytes = lines[i].bytes < sizeof(text) ? lines[i].bytes : sizeof(text) - 1;
        memcpy(text, lines[i].start, bytes);
        text[bytes] = '\0';
        ui__text(text, r.x + UI_BUBBLE_PAD, r.y + UI_BUBBLE_PAD + (int)i * ui__line_height(), ink);
    }
    char when[16];
    ui__format_when(message->utc, ui->now_utc, ui->utc_offset, when, sizeof(when));
    if (message->incoming) ui__text(when, r.x + 2, time_y, p->muted);
    else ui__right_text(when, r.x + r.w - 2, time_y, p->muted);
}

static void ui__draw_thread(const ui_t *ui, int width, int height) {
    const ui_palette_t *p = ui__palette(ui);
    size_t indices[STORE_MESSAGES_MAX];
    size_t count = store__thread_messages(ui->store, ui->number, indices, STORE_MESSAGES_MAX);
    int top = ui__thread_top(), bottom = ui__thread_bottom(height);
    /* Newest at the bottom; bubbles cut by the bars are left out. */
    int y = bottom + ui->thread_scroll;
    for (size_t i = count; i-- > 0 && y > top;) {
        const store_message_t *message = &ui->store->messages[indices[i]];
        int h = ui__bubble_height(message, width);
        if (y - h >= top && y <= bottom) ui__draw_bubble(ui, message, width, y);
        y -= h;
    }
    if (count == 0) ui__centre_text("No messages yet", width / 2, (top + bottom) / 2 - s_ch / 2, p->muted);

    /* Header: back and who. */
    ui__fill_rect(0, 0, width, ui__bar_height(), p->bg);
    ui__icon("arrow-left", UI_GAP, 2, s_ch + 4, p->text);
    char name[64];
    ui__fit(ui__display_name(ui, ui->number), (width - 3 * UI_GAP - s_ch - 4) / s_cw, name, sizeof(name));
    ui__text(name, 2 * UI_GAP + s_ch + 4, 4, p->text);
    ui__fill_rect(0, ui__bar_height() - 1, width, 1, ui->epaper ? p->outline : p->surface);

    /* Compose bar. */
    ui__fill_rect(0, bottom, width, height - bottom, p->bg);
    ui__fill_rect(0, bottom, width, 1, ui->epaper ? p->outline : p->surface);
    ui_rect_t box = ui__compose_rect(width, height);
    ui__round_rect(box, box.h / 2, p->surface, true);
    if (ui->epaper) ui__round_rect(box, box.h / 2, p->outline, false);
    char count_text[16];
    ui__draft_count(ui->draft, count_text, sizeof(count_text));
    bool fits = ui__draft_fits(ui->draft);
    int count_w = (int)strlen(count_text) * s_cw;
    int text_x = box.x + box.h / 2, text_y = box.y + (box.h - s_ch) / 2;
    int max_chars = (box.w - box.h - count_w - UI_GAP) / s_cw;
    if (ui->sending) {
        ui__text("Sending...", text_x, text_y, p->muted);
    } else if (ui->draft[0] == '\0') {
        ui__text("Type a message", text_x, text_y, p->muted);
    } else {
        /* The end of the draft, where typing happens. */
        size_t chars = ui__chars(ui->draft, strlen(ui->draft));
        const char *tail = ui->draft;
        while (max_chars > 0 && chars > (size_t)max_chars) {
            tail += ui__char_bytes(tail);
            chars--;
        }
        ui__text(tail, text_x, text_y, p->text);
        ui__right_text(count_text, box.x + box.w - box.h / 2, text_y, fits ? p->muted : p->text);
    }
    ui_rect_t send = ui__send_rect(width, height);
    bool ready = ui->draft[0] != '\0' && fits && !ui->sending;
    ui__disc(send.x + send.w / 2, send.y + send.h / 2, send.w / 2, ready ? p->accent : p->surface);
    if (ui->epaper && !ready)
        display__draw_circle(
            s_origin_x + send.x + send.w / 2, s_origin_y + send.y + send.h / 2, send.w / 2, p->outline
        );
    int icon = send.w / 2;
    ui__icon(
        "send",
        send.x + (send.w - icon) / 2,
        send.y + (send.h - icon) / 2,
        icon,
        ready ? p->on_accent : p->muted
    );
}

void ui__draw(const ui_t *ui, int width, int height) {
    ui__fill_rect(0, 0, width, height, ui__palette(ui)->bg);
    display__set_text_size(1);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    if (ui->screen == UI_SCREEN_THREAD) ui__draw_thread(ui, width, height);
    else ui__draw_list(ui, width, height);
}
