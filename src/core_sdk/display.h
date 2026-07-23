#pragma once

/*
 * Public display HAL API.
 *
 * This is the Core-side implementation of the JavaScript `display.*`
 * surface used by the bjs_interpreter.  It is intentionally small and
 * immediate-mode: drawing primitives update a Core-owned framebuffer and
 * are sent to the LCD only when display__flush() is called.  Display access
 * is serialized by a Core mutex so tasks share one output surface safely.
 *
 * All coordinates are logical and respect the current rotation set by
 * display__set_rotation().  The framebuffer is stored in the logical
 * orientation for the current rotation; the ST7789 controller maps it to the
 * physical panel via esp_lcd_panel_swap_xy() / esp_lcd_panel_mirror().  The
 * default rotation is board-specific (portrait for M5 StickC Plus2, landscape
 * for M5 Cardputer).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default logical dimensions after applying the board's default rotation. */
#define BRUCE_DISPLAY_NATIVE_WIDTH 135
#define BRUCE_DISPLAY_NATIVE_HEIGHT 240

/* Some common RGB565 colors matching the legacy JS display constants. */
#define BRUCE_COLOR_BLACK 0x0000
#define BRUCE_COLOR_NAVY 0x000F
#define BRUCE_COLOR_DARKGREEN 0x03E0
#define BRUCE_COLOR_DARKCYAN 0x03EF
#define BRUCE_COLOR_MAROON 0x7800
#define BRUCE_COLOR_PURPLE 0x780F
#define BRUCE_COLOR_OLIVE 0x7BE0
#define BRUCE_COLOR_LIGHTGREY 0xC618
#define BRUCE_COLOR_DARKGREY 0x7BEF
#define BRUCE_COLOR_BLUE 0x001F
#define BRUCE_COLOR_GREEN 0x07E0
#define BRUCE_COLOR_CYAN 0x07FF
#define BRUCE_COLOR_RED 0xF800
#define BRUCE_COLOR_MAGENTA 0xF81F
#define BRUCE_COLOR_YELLOW 0xFFE0
#define BRUCE_COLOR_WHITE 0xFFFF
#define BRUCE_COLOR_ORANGE 0xFD20
#define BRUCE_COLOR_GREENYELLOW 0xAFE5
#define BRUCE_COLOR_PINK 0xF81F
#define BRUCE_COLOR_TRANSPARENT 0x10000

typedef uint16_t bruce_display_color_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Initialize the LCD controller, backlight, and framebuffer.  Safe to call
 * more than once; subsequent calls return BRUCE_OK without re-initializing.
 * This is normally called once by Core boot (main.c) before the launcher
 * starts; applications do not need to call it.
 */
bruce_result_t display__init(void);

/* Release the LCD panel, SPI bus, backlight PWM, and framebuffer. */
void display__deinit(void);

/* -------------------------------------------------------------------------- */
/* Screen dimensions and colors                                               */
/* -------------------------------------------------------------------------- */

/* Logical width in the current rotation.  Always returns a positive value. */
int display__width(void);

/* Logical height in the current rotation.  Always returns a positive value. */
int display__height(void);

/* Convert 8-bit RGB components to an RGB565 color value. */
bruce_display_color_t display__color565(uint8_t r, uint8_t g, uint8_t b);

/* Legacy alias for display__color565(). */
static inline bruce_display_color_t display__color(uint8_t r, uint8_t g, uint8_t b)
{
    return display__color565(r, g, b);
}

/* -------------------------------------------------------------------------- */
/* Screen fill / clear                                                        */
/* -------------------------------------------------------------------------- */

/* Fill the entire logical screen with `color`. */
bruce_result_t display__fill_screen(bruce_display_color_t color);

/* Fill the screen with the current background color (black by default). */
bruce_result_t display__clear(void);

/* -------------------------------------------------------------------------- */
/* Text state and output                                                      */
/* -------------------------------------------------------------------------- */

/* Set the foreground color used by subsequent text and bitmap drawing. */
bruce_result_t display__set_text_color(bruce_display_color_t color);

/*
 * Set the background color used for erasing behind text.
 * Pass a value >= 0x10000 (e.g. BRUCE_COLOR_TRANSPARENT) for transparent
 * background rendering; otherwise the background rectangle is filled.
 * The parameter is 32-bit so the transparent sentinel fits.
 */
bruce_result_t display__set_text_bg_color(uint32_t color);

/* Text size multiplier (1 = 8x16 glyphs, 2 = 16x32, ...).  Clamped to 1..8. */
bruce_result_t display__set_text_size(uint8_t size);

/* Position the text cursor in logical coordinates. */
bruce_result_t display__set_cursor(int16_t x, int16_t y);

/* Read the current text cursor position in logical coordinates. */
bruce_result_t display__get_cursor(int16_t *x, int16_t *y);

/* Print a NUL-terminated UTF-8 string at the cursor and advance the cursor. */
bruce_result_t display__print(const char *text);

/* Same as display__print() but appends a newline. */
bruce_result_t display__println(const char *text);

/* -------------------------------------------------------------------------- */
/* Primitive drawing                                                          */
/* -------------------------------------------------------------------------- */

bruce_result_t display__draw_pixel(int16_t x, int16_t y, bruce_display_color_t color);
bruce_result_t display__draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                   bruce_display_color_t color);
bruce_result_t display__draw_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                   bruce_display_color_t color);
bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                   bruce_display_color_t color);
bruce_result_t display__draw_circle(int16_t x, int16_t y, int16_t r,
                                     bruce_display_color_t color);
bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r,
                                     bruce_display_color_t color);
bruce_result_t display__draw_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                       int16_t x2, int16_t y2, bruce_display_color_t color);
bruce_result_t display__fill_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                       int16_t x2, int16_t y2, bruce_display_color_t color);
bruce_result_t display__draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                         int16_t r, bruce_display_color_t color);
bruce_result_t display__fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                         int16_t r, bruce_display_color_t color);

/* -------------------------------------------------------------------------- */
/* Bitmaps                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * Draw a 1bpp MSB-first bitmap.  Each row is byte-aligned; a set bit draws
 * `color`, a clear bit draws the current background color (or is skipped
 * when the background is transparent).
 */
bruce_result_t display__draw_bitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                                     int16_t w, int16_t h, bruce_display_color_t color);

/*
 * Draw a 1bpp XBM bitmap.  Each row is byte-aligned; a set bit draws `color`,
 * a clear bit is transparent.
 */
bruce_result_t display__draw_xbitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                                      int16_t w, int16_t h, bruce_display_color_t color);

/* Draw a raw RGB565 bitmap from `bitmap` (width * height * 2 bytes). */
bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap,
                                         int16_t w, int16_t h);

/* -------------------------------------------------------------------------- */
/* Display control                                                            */
/* -------------------------------------------------------------------------- */

/* Set rotation: 0 = native, 1 = 90deg clockwise, 2 = 180, 3 = 270. */
bruce_result_t display__set_rotation(uint8_t rotation);

/* Return the current rotation (0..3). */
uint8_t display__get_rotation(void);

/* Invert the panel colors (independent of color_inverted config). */
bruce_result_t display__invert_display(bool invert);

/* Set backlight brightness, 0..255.  Persists via config__set_bright(). */
bruce_result_t display__set_brightness(uint8_t brightness);

/* Return the current backlight brightness, 0..255. */
uint8_t display__get_brightness(void);

/* Turn the panel display on or off (backlight is controlled separately). */
bruce_result_t display__display_on_off(bool on);

/* -------------------------------------------------------------------------- */
/* Framebuffer flush                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Send the framebuffer contents to the LCD panel.  Drawing primitives only
 * update the in-memory framebuffer; this call makes them visible.  Returns
 * BRUCE_OK or BRUCE_ERR_NOT_INITIALIZED / BRUCE_ERR_BUSY.
 */
bruce_result_t display__flush(void);

#ifdef __cplusplus
}
#endif
