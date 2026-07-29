#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/display.h"

bruce_result_t display_driver__init(void);
void display_driver__deinit(void);
void display_driver__configure_rotation(uint8_t rotation);
bruce_result_t display_driver__draw_bitmap(
    int x_start, int y_start, int x_end, int y_end, const bruce_display_color_t *pixels
);
void display_driver__set_backlight(uint8_t brightness);
bruce_result_t display_driver__invert(bool invert);
bruce_result_t display_driver__set_enabled(bool enabled);
