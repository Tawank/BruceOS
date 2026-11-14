#include "clock_ui.h"

#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_UI__GLYPH_W 6
#define CLOCK_UI__GLYPH_H 10
/* Glyph ink spans rows 2-8 of the 10-row cell, so its centre sits half a row below the cell's. */
#define CLOCK_UI__GLYPH_INK_H 11
#define CLOCK_UI__ICON_MAX 20
#define CLOCK_UI__CORNER 3

static bool clock_ui__is_fixed(const clock_ui_t *ui, const clock_item_t *item) { return item->y >= ui->viewport_h; }

static int16_t clock_ui__row_offset(const clock_ui_t *ui, const clock_item_t *item) {
    return clock_ui__is_fixed(ui, item) ? 0 : ui->scroll;
}

static size_t clock_ui__focusable_count(const clock_ui_t *ui) {
    size_t count = 0;
    for (size_t i = 0; i < ui->count; ++i) count += ui->items[i].button;
    return count;
}

static const clock_item_t *clock_ui__focusable_at(const clock_ui_t *ui, size_t ordinal) {
    for (size_t i = 0; i < ui->count; ++i) {
        if (ui->items[i].button && ordinal-- == 0) return &ui->items[i];
    }
    return NULL;
}

static clock_item_t *clock_ui__add(clock_ui_t *ui, int16_t x, int16_t y, int16_t w, int16_t h, const char *label) {
    if (ui->count == CLOCK_UI__MAX_ITEMS) return NULL;
    clock_item_t *item = &ui->items[ui->count++];
    memset(item, 0, sizeof(*item));
    item->x = x;
    item->y = y;
    item->w = w;
    item->h = h;
    item->text_size = 1;
    if (label != NULL) snprintf(item->label, sizeof(item->label), "%s", label);
    return item;
}

void clock_ui__begin(clock_ui_t *ui, int16_t viewport_h) {
    ui->count = 0;
    ui->viewport_h = viewport_h;
}

clock_item_t *clock_ui__label(clock_ui_t *ui, int16_t x, int16_t y, int16_t w, int16_t h, const char *text) {
    return clock_ui__add(ui, x, y, w, h, text);
}

clock_item_t *clock_ui__button(
    clock_ui_t *ui, int16_t x, int16_t y, int16_t w, int16_t h, const char *label, const char *icon, int action, int arg
) {
    clock_item_t *item = clock_ui__add(ui, x, y, w, h, label);
    if (item == NULL) return NULL;
    item->icon = icon;
    item->action = action;
    item->arg = arg;
    item->button = true;
    return item;
}

uint8_t clock_ui__fit_text_size(const char *text, int16_t w, int16_t h, uint8_t max_size) {
    int fit_w = w / ((int)strlen(text) * CLOCK_UI__GLYPH_W);
    int fit_h = h / CLOCK_UI__GLYPH_H;
    int size = fit_w < fit_h ? fit_w : fit_h;
    if (size > max_size) size = max_size;
    return size < 1 ? 1 : (uint8_t)size;
}

void clock_ui__focus_step(clock_ui_t *ui, int step) {
    size_t count = clock_ui__focusable_count(ui);
    if (count == 0) return;
    ui->focus = (ui->focus + count + (size_t)(step % (int)count)) % count;
}

void clock_ui__focus_row(clock_ui_t *ui, int direction) {
    const clock_item_t *from = clock_ui__focusable_at(ui, ui->focus);
    if (from == NULL) return;
    long best_score = -1;
    size_t best = ui->focus;
    for (size_t i = 0, ordinal = 0; i < ui->count; ++i) {
        const clock_item_t *item = &ui->items[i];
        if (!item->button) continue;
        int dy = (item->y + item->h / 2) - (from->y + from->h / 2);
        int dx = abs((item->x + item->w / 2) - (from->x + from->w / 2));
        long score = (long)abs(dy) * 1024 + dx;
        if (dy * direction > from->h / 2 && (best_score < 0 || score < best_score)) {
            best_score = score;
            best = ordinal;
        }
        ordinal++;
    }
    ui->focus = best;
}

const clock_item_t *clock_ui__focused(const clock_ui_t *ui) {
    size_t count = clock_ui__focusable_count(ui);
    return count == 0 ? NULL : clock_ui__focusable_at(ui, ui->focus % count);
}

static void clock_ui__reveal_focus(clock_ui_t *ui) {
    const clock_item_t *item = clock_ui__focused(ui);
    if (item == NULL || clock_ui__is_fixed(ui, item)) return;
    if (item->y < ui->scroll) ui->scroll = item->y;
    if (item->y + item->h > ui->scroll + ui->viewport_h) ui->scroll = item->y + item->h - ui->viewport_h;
}

static uint16_t clock_ui__tone_color(clock_tone_t tone) {
    switch (tone) {
    case CLOCK_TONE_PRIMARY: return config__get_color_primary();
    case CLOCK_TONE_MUTED: return config__get_color_text_muted();
    default: return config__get_color_text();
    }
}

static void clock_ui__draw_text(const clock_item_t *item, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    int top = y + h / 2 - item->text_size * CLOCK_UI__GLYPH_INK_H / 2;
    (void)display__set_text_color(color);
    (void)display__set_text_size((float)item->text_size);
    (void)display__draw_centre_string(item->label, x + w / 2, top);
}

static void clock_ui__draw_button(const clock_item_t *item, int16_t y, bool focused) {
    uint16_t fill = focused ? config__get_color_primary() : config__get_color_surface();
    uint16_t ink = focused ? config__get_color_background() : config__get_color_text();
    (void)display__fill_round_rect(item->x, y, item->w, item->h, CLOCK_UI__CORNER, fill);
    if (item->current) {
        (void)display__draw_round_rect(item->x, y, item->w, item->h, CLOCK_UI__CORNER, config__get_color_secondary());
    }
    const bruce_icon_t *icon = item->icon != NULL ? icon__get(item->icon) : NULL;
    bool stacked = icon != NULL && item->label[0] != '\0';
    int16_t label_h = stacked ? CLOCK_UI__GLYPH_H + 2 : item->h;
    if (icon != NULL) {
        int16_t size = item->h - (stacked ? label_h : 0) - 4;
        if (size > CLOCK_UI__ICON_MAX) size = CLOCK_UI__ICON_MAX;
        int16_t area_h = item->h - (stacked ? label_h : 0);
        (void)display__draw_bitmap_scaled(
            item->x + (item->w - size) / 2, y + (area_h - size) / 2, icon->bits, icon->width, icon->height, size, size,
            ink
        );
    }
    if (item->label[0] != '\0') {
        (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
        clock_ui__draw_text(item, item->x, stacked ? y + item->h - label_h : y, item->w, label_h, ink);
    }
}

bruce_result_t clock_ui__draw(clock_ui_t *ui) {
    clock_ui__reveal_focus(ui);
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    (void)display__fill_screen(config__get_color_background());
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    const clock_item_t *focused = clock_ui__focused(ui);
    for (size_t i = 0; i < ui->count; ++i) {
        const clock_item_t *item = &ui->items[i];
        int16_t y = item->y - clock_ui__row_offset(ui, item);
        bool fixed = clock_ui__is_fixed(ui, item);
        if (!fixed && (y < 0 || y + item->h > ui->viewport_h)) continue;
        if (item->button) clock_ui__draw_button(item, y, item == focused);
        else clock_ui__draw_text(item, item->x, y, item->w, item->h, clock_ui__tone_color(item->tone));
    }
    (void)display__set_text_size(1);
    return display__present();
}

static uint32_t clock_ui__mix(uint32_t hash, uint32_t value) { return (hash ^ value) * 16777619u; }

uint32_t clock_ui__signature(const clock_ui_t *ui) {
    uint32_t hash = clock_ui__mix(2166136261u, (uint32_t)ui->focus);
    hash = clock_ui__mix(hash, (uint32_t)ui->scroll);
    for (size_t i = 0; i < ui->count; ++i) {
        const clock_item_t *item = &ui->items[i];
        for (const char *c = item->label; *c != '\0'; ++c) hash = clock_ui__mix(hash, (uint8_t)*c);
        hash = clock_ui__mix(hash, (uint32_t)(uintptr_t)item->icon);
        hash = clock_ui__mix(hash, ((uint32_t)(uint16_t)item->y << 16) | (uint16_t)item->x);
        hash = clock_ui__mix(hash, item->text_size);
    }
    return hash;
}
