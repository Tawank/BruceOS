#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define CLOCK_UI__MAX_ITEMS 40
#define CLOCK_UI__LABEL_MAX 16

typedef enum {
    CLOCK_TONE_TEXT,
    CLOCK_TONE_PRIMARY,
    CLOCK_TONE_MUTED,
} clock_tone_t;

typedef struct {
    int16_t x, y, w, h;
    char label[CLOCK_UI__LABEL_MAX];
    const char *icon;
    int action;
    int arg;
    uint8_t text_size;
    clock_tone_t tone;
    bool button;
    bool current;
} clock_item_t;

typedef struct {
    clock_item_t items[CLOCK_UI__MAX_ITEMS];
    size_t count;
    size_t focus;
    int16_t scroll;
    int16_t viewport_h;
} clock_ui_t;

/* Items below `viewport_h` are fixed and never scroll (the bottom bar). */
void clock_ui__begin(clock_ui_t *ui, int16_t viewport_h);

clock_item_t *clock_ui__label(clock_ui_t *ui, int16_t x, int16_t y, int16_t w, int16_t h, const char *text);
clock_item_t *clock_ui__button(
    clock_ui_t *ui, int16_t x, int16_t y, int16_t w, int16_t h, const char *label, const char *icon, int action, int arg
);

/* Largest text size, up to `max_size`, at which `text` fits in w x h. */
uint8_t clock_ui__fit_text_size(const char *text, int16_t w, int16_t h, uint8_t max_size);

void clock_ui__focus_step(clock_ui_t *ui, int step);
void clock_ui__focus_row(clock_ui_t *ui, int direction);
const clock_item_t *clock_ui__focused(const clock_ui_t *ui);

/* Changes whenever anything visible changes, so callers can skip identical frames. */
uint32_t clock_ui__signature(const clock_ui_t *ui);

bruce_result_t clock_ui__draw(clock_ui_t *ui);
