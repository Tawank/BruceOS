#pragma once

/* LVGL's LV_ASSERT_HANDLER: ends the calling process instead of halting the
 * whole device (core/lvgl/lvgl_port.c). */
__attribute__((noreturn)) void lvgl__assert_failed(void);
