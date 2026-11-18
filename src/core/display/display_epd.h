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
