#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/cellular.h"
#include "store.h"

typedef enum {
    UI_SCREEN_LIST,
    UI_SCREEN_THREAD,
} ui_screen_t;

typedef enum {
    UI_HIT_NONE,
    UI_HIT_ROW,
    UI_HIT_BACK,
    UI_HIT_COMPOSE,
    UI_HIT_SEND,
} ui_hit_kind_t;

typedef struct {
    ui_hit_kind_t kind;
    int index;
} ui_hit_t;

typedef struct {
    int x, y, w, h;
} ui_rect_t;

/* One wrapped line: a slice of the text. */
typedef struct {
    const char *start;
    size_t bytes;
} ui_line_t;

typedef struct {
    ui_screen_t screen;
    const store_t *store;
    store_thread_t threads[STORE_MESSAGES_MAX];
    size_t thread_count;
    int selected;                           /* List row; row 0 is "New message". */
    int scroll;                             /* First list row shown. */
    char number[BRUCE_CELLULAR_NUMBER_MAX]; /* The open conversation. */
    int thread_scroll;                      /* Pixels scrolled up from the newest message. */
    char draft[BRUCE_CELLULAR_TEXT_MAX];
    bool sending;
    bruce_cellular_status_t status;
    int64_t now_utc;
    int64_t utc_offset;
    bool epaper;
} ui_t;

/* Reads the font size; call before drawing or hit testing. */
void ui__init_metrics(void);
/* Screen position of the content area that drawing is relative to. */
void ui__set_origin(int x, int y);
void ui__draw(const ui_t *ui, int width, int height);
ui_hit_t ui__hit(const ui_t *ui, int width, int height, int x, int y);

int ui__visible_rows(int height);
ui_rect_t ui__row_rect(int visible_index, int width);
ui_rect_t ui__compose_rect(int width, int height);
ui_rect_t ui__send_rect(int width, int height);
void ui__scroll_to_selection(ui_t *ui, int height);
/* How far the open conversation can scroll up. */
int ui__thread_max_scroll(const ui_t *ui, int width, int height);
int ui__thread_step(void);

/* Word-wraps text to max_chars characters per line; returns the line count. */
size_t ui__wrap(const char *text, int max_chars, ui_line_t *lines, size_t capacity);
/* "12/160": characters typed and what one message holds. */
void ui__draft_count(const char *draft, char *out, size_t capacity);
bool ui__draft_fits(const char *draft);
void ui__format_when(int64_t utc, int64_t now_utc, int64_t offset, char *out, size_t capacity);
const char *ui__state_text(bruce_cellular_state_t state);
