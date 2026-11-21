#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "book.h"
#include "core_sdk/cellular.h"

#define UI_KEY_COUNT 12
#define UI_DTMF_MAX 24

typedef enum {
    UI_TAB_KEYPAD,
    UI_TAB_RECENTS,
    UI_TAB_CONTACTS,
    UI_TAB_COUNT,
} ui_tab_t;

typedef enum {
    UI_HIT_NONE,
    UI_HIT_TAB,
    UI_HIT_KEY,
    UI_HIT_BACKSPACE,
    UI_HIT_CALL,
    UI_HIT_SAVE_NUMBER,
    UI_HIT_ROW,
    UI_HIT_ANSWER,
    UI_HIT_DECLINE,
    UI_HIT_HANG_UP,
    UI_HIT_MUTE,
    UI_HIT_DTMF_PAD,
    UI_HIT_VOLUME,
} ui_hit_kind_t;

typedef struct {
    ui_hit_kind_t kind;
    int index;
} ui_hit_t;

typedef struct {
    int x, y, w, h;
} ui_rect_t;

typedef struct {
    ui_tab_t tab;
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    int selected; /* Row in the list; on Contacts row 0 is "New contact". */
    int scroll;   /* First row shown. */
    bool dtmf_pad;
    char dtmf[UI_DTMF_MAX];
    const book_t *book;
    bruce_cellular_status_t status;
    int64_t now_utc;
    int64_t utc_offset;
    bool epaper;
} ui_t;

/* Reads the font size; call before drawing or hit testing. */
void ui__init_metrics(void);

bool ui__in_call(const ui_t *ui);
void ui__draw(const ui_t *ui, int width, int height);
ui_hit_t ui__hit(const ui_t *ui, int width, int height, int x, int y);

size_t ui__row_count(const ui_t *ui);
int ui__visible_rows(int height);
/* Keeps the selected row on screen. */
void ui__scroll_to_selection(ui_t *ui, int height);

/* "1".."9", "*", "0", "#". */
char ui__key_char(int index);

void ui__format_duration(uint32_t seconds, char *out, size_t capacity);
/* "14:05" today, "Yesterday", a weekday within a week, else "21.11". */
void ui__format_when(int64_t utc, int64_t now_utc, int64_t offset, char *out, size_t capacity);
const char *ui__state_text(bruce_cellular_state_t state);
const char *ui__call_text(const bruce_cellular_status_t *status);

/* Layout, shared by drawing and hit testing. */
ui_rect_t ui__tab_rect(int tab, int width, int height);
ui_rect_t ui__key_rect(int index, int width, int height);
ui_rect_t ui__call_button_rect(int width, int height);
ui_rect_t ui__backspace_rect(int width, int height);
ui_rect_t ui__save_number_rect(int width, int height);
ui_rect_t ui__row_rect(int visible_index, int width, int height);
ui_rect_t ui__call_screen_rect(ui_hit_kind_t button, bool incoming, int width, int height);
ui_rect_t ui__dtmf_key_rect(int index, int width, int height);
