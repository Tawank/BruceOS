#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/display.h"

/* UC8253 e-paper panel behind display_driver.c (BRUCE_DISPLAY_PANEL_UC8253). */
bruce_result_t display_epd__init(void);
void display_epd__deinit(void);
void display_epd__configure_rotation(uint8_t rotation);
bruce_result_t display_epd__draw_bitmap(
    int x_start, int y_start, int x_end, int y_end, const bruce_display_color_t *pixels
);
bruce_result_t display_epd__invert(bool invert);
bruce_result_t display_epd__set_enabled(bool enabled);
void display_epd__request_full_refresh(void);
/* A presented frame is waiting in the framebuffer; the panel task pulls it before its next refresh. */
void display_epd__frame_ready(void);
/* See display__wait_shown() and display__freeze(). */
bruce_result_t display_epd__wait_shown(uint32_t timeout_ms);
void display_epd__freeze(bool frozen);
