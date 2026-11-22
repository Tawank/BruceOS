#pragma once

#include <stdint.h>

#include "core_sdk/input.h"
#include "core_sdk/result.h"
/* Angle brackets: "lvgl.h" would find this file first. */
#include <lvgl.h>

/**
 * @file
 * @brief LVGL for apps.
 *
 * Each process gets its own LVGL: its own objects, styles, timers and input,
 * drawn into the process's viewport, so several apps can show LVGL at once
 * (the process preview, split views). Call every lv_* function from the task
 * that called lvgl__begin(). Memory comes from the process and is freed when
 * it exits. The configuration is core_sdk/lvgl/lv_conf.h.
 *
 * A typical loop:
 *
 *     lvgl__begin();
 *     ... create widgets on lv_screen_active() ...
 *     while (running) {
 *         bruce_input_event_t event;
 *         while (input__poll(&event) == BRUCE_OK) {
 *             if (!handled_by_app(&event)) lvgl__input(&event);
 *         }
 *         runtime__delay(lvgl__step());
 *     }
 *     lvgl__end();
 */

/**
 * @brief Sets up LVGL for the calling process.
 *
 * Creates a display the size of the process's viewport with Bruce's look
 * (the configured theme colors, Bruce's font, flat buttons that fill with the
 * primary color when focused and get a secondary outline when checked), a
 * keypad whose keys go to the default group (lv_group_get_default()), and a
 * pointer for touch screens. BRUCE_OK when already set up.
 */
bruce_result_t lvgl__begin(void);

/**
 * @brief Hands an input event to LVGL.
 *
 * Arrows, Select (Enter), Back (Esc), Prev/Next (focus) and typed characters
 * go to the keypad; touches go to the pointer.
 */
void lvgl__input(const bruce_input_event_t *event);

/**
 * @brief Runs LVGL's timers and draws what changed.
 *
 * Follows viewport changes (moved, resized, shown again) and skips drawing
 * while the process is hidden. Returns how many ms LVGL can wait before the
 * next call.
 */
uint32_t lvgl__step(void);

/**
 * @brief Frees the calling process's LVGL; lvgl__begin() may follow again.
 */
void lvgl__end(void);

/**
 * @brief Bruce's own bitmap font, scaled up by whole pixels like
 * display__set_text_size(): 1 is the default text, up to 4.
 *
 * LVGL's symbols (LV_SYMBOL_*) come from Montserrat as a fallback.
 */
const lv_font_t *lvgl__font(int scale);

/** @brief An LVGL color from a Bruce RGB565 color, e.g. config__get_color_text_muted(). */
lv_color_t lvgl__color(uint16_t rgb565);

/**
 * @brief Shows a built-in icon (core_sdk/icon.h) at `size` x `size` pixels.
 *
 * The icon takes the text color of the object, inherited from its parent, so
 * an icon on a button changes with the button's focus. NULL for an unknown
 * name.
 */
lv_obj_t *lvgl__icon_create(lv_obj_t *parent, const char *name, int32_t size);
