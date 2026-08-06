#pragma once

/*
 * Public display HAL API.
 *
 * This is the Core-side implementation of the JavaScript `display.*`
 * surface used by the bjs_interpreter.  It is intentionally small and
 * immediate-mode: buffered drawing updates a Core-owned framebuffer and is
 * sent to the LCD when a frame is presented; direct drawing is streamed to
 * the panel immediately. GUI processes render in
 * process-local coordinates into a Core-assigned fullscreen or tiled viewport.
 * Hidden processes see a zero-sized viewport and drawing is a successful no-op.
 *
 * All coordinates are logical and respect the current rotation set by
 * display__set_rotation().  The framebuffer is stored in the logical
 * orientation for the current rotation; the ST7789 controller maps it to the
 * physical panel via esp_lcd_panel_swap_xy() / esp_lcd_panel_mirror().  The
 * default rotation is set per board by CONFIG_BRUCE_DISPLAY_DEFAULT_ROTATION
 * (see src/Kconfig.projbuild and boards/<board>/sdkconfig.defaults).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Common RGB565 colors. */
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
#define BRUCE_COLOR_TRANSPARENT 0x10000

typedef uint16_t bruce_display_color_t;

#define BRUCE_DISPLAY_MAX_TILES 4

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
} bruce_display_rect_t;

typedef struct {
    bruce_process_id_t process_id;
    bruce_display_rect_t rect;
} bruce_display_tile_t;

/* -------------------------------------------------------------------------- */
/* Screen dimensions and colors                                               */
/* -------------------------------------------------------------------------- */

/* Width of the caller's current viewport, or zero while hidden. */
int display__width(void);

/* Height of the caller's current viewport, or zero while hidden. */
int display__height(void);

/* Full physical screen width/height, independent of the caller's own
 * viewport (which may be smaller, tiled, or zero while hidden/background).
 * Meant for positioning an overlay (see "Overlays" below): an overlay owner
 * commonly has no normal viewport of its own to measure against. Zero
 * before display__init(). */
int display__screen_width(void);
int display__screen_height(void);

/* Convert 8-bit RGB components to an RGB565 color value. */
bruce_display_color_t display__color565(uint8_t r, uint8_t g, uint8_t b);

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

/* Text size multiplier (1 = 6x8 character cells, 2 = 12x16, ...). Clamped to 1..8. */
bruce_result_t display__set_text_size(uint8_t size);

/* Position the text cursor in logical coordinates. */
bruce_result_t display__set_cursor(int16_t x, int16_t y);

/* Read the current text cursor position in logical coordinates. */
bruce_result_t display__get_cursor(int16_t *x, int16_t *y);

/* Print a NUL-terminated UTF-8 string at the cursor and advance the cursor. */
bruce_result_t display__print(const char *text);

/* Same as display__print() but appends a newline. */
bruce_result_t display__println(const char *text);

/* Draw a single-line string with (x, y) at its left edge. */
bruce_result_t display__draw_string(const char *text, int16_t x, int16_t y);

/* Draw a single-line string horizontally centered on x. */
bruce_result_t display__draw_centre_string(const char *text, int16_t x, int16_t y);

/* Draw a single-line string with its right edge at x. */
bruce_result_t display__draw_right_string(const char *text, int16_t x, int16_t y);

/* -------------------------------------------------------------------------- */
/* Primitive drawing                                                          */
/* -------------------------------------------------------------------------- */

bruce_result_t display__draw_pixel(int16_t x, int16_t y, bruce_display_color_t color);
bruce_result_t
display__draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bruce_display_color_t color);
bruce_result_t display__draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color);
bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color);
bruce_result_t display__draw_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color);
bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color);
/* Draw a circular arc. Angles use the legacy Bruce convention: zero degrees
 * is at six o'clock and values increase clockwise. An end angle less than the
 * start angle wraps through zero; a range of 360 degrees draws a full circle. */
bruce_result_t display__draw_arc(
    int16_t x, int16_t y, int16_t r, int16_t start_angle, int16_t end_angle, bruce_display_color_t color
);
bruce_result_t display__draw_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
);
bruce_result_t display__fill_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
);
bruce_result_t
display__draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color);
bruce_result_t
display__fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color);

/* -------------------------------------------------------------------------- */
/* Bitmaps                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * Draw a 1bpp MSB-first bitmap.  Each row is byte-aligned; a set bit draws
 * `color`, a clear bit draws the current background color (or is skipped
 * when the background is transparent).
 */
bruce_result_t display__draw_bitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
);

/*
 * Draw a 1bpp XBM bitmap.  Each row is byte-aligned; a set bit draws `color`,
 * a clear bit is transparent.
 */
bruce_result_t display__draw_xbitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
);

/* Draw a raw RGB565 bitmap from `bitmap` (width * height * 2 bytes). */
bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);

/*
 * Draw a 1bpp MSB-first bitmap of `w` x `h` pixels scaled to `dw` x `dh`
 * using nearest-neighbor sampling.  A set bit draws `color`; a clear bit is
 * transparent.  This is the fast path for built-in icons (see icon.h): pure
 * integer math, no parsing, no extra RAM.
 */
bruce_result_t display__draw_bitmap_scaled(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, int16_t dw, int16_t dh,
    bruce_display_color_t color
);

/* -------------------------------------------------------------------------- */
/* Display control                                                            */
/* -------------------------------------------------------------------------- */

/* Set rotation: 0 = native, 1 = 90deg clockwise, 2 = 180, 3 = 270. */
bruce_result_t display__set_rotation(uint8_t rotation);

/* Return the current rotation (0..3). */
uint8_t display__get_rotation(void);

/* Copies the composed RGB565 framebuffer for remote display tools. Pass NULL
 * with capacity 0 to query dimensions and required pixel count. Returns
 * BRUCE_ERR_UNSUPPORTED when buffered rendering is disabled. */
bruce_result_t display__snapshot(
    uint16_t *pixels,
    size_t capacity,
    uint16_t *out_width,
    uint16_t *out_height,
    size_t *out_pixel_count
);

/* Invert the panel colors (independent of color_inverted config). */
bruce_result_t display__invert_display(bool invert);

/* Set backlight brightness, 0..255.  Persists via config__set_bright(). */
bruce_result_t display__set_brightness(uint8_t brightness);

/* Return the current backlight brightness, 0..255. */
uint8_t display__get_brightness(void);

/* Turn the panel display on or off (backlight is controlled separately). */
bruce_result_t display__display_on_off(bool on);

/*
 * Frees the off-screen DMA framebuffer and switches rendering to direct
 * (unbuffered) mode, or reverts back -- entirely at runtime, no reboot
 * required. Meant to be called by a memory-hungry ELF/JS app (an emulator, a
 * game with a large asset set, ...) right before it starts allocating, to
 * hand back the RAM the buffered/DMA framebuffer would otherwise be holding
 * for the whole time the app runs.
 *
 * display__game_mode(true) tears down the current s_framebuffer/pack buffer
 * (if buffered rendering was in use) and allocates only the small direct-mode
 * scratch buffer instead; the calling process becomes game mode's owner.
 * display__game_mode(false), called by that same owning process, restores
 * whatever rendering mode was active before game mode was turned on. Calling
 * it again with the same value the caller already holds is a no-op. Only the
 * owning process may turn its own game mode back off; if the owner exits (or
 * is killed) without doing so, the display core reverts it automatically.
 *
 * Fails with BRUCE_ERR_BUSY if another process currently owns game mode, or
 * while any process has an active (begun but not yet presented) frame.
 */
bruce_result_t display__game_mode(bool enable);

/* -------------------------------------------------------------------------- */
/* Framebuffer flush                                                          */
/* -------------------------------------------------------------------------- */

/* Lease the caller's current viewport for one complete frame. In direct mode,
 * drawing is visible before the frame is presented. */
bruce_result_t display__begin_frame(void);

/* Present an active frame and wait until its LCD transfer completes. Direct
 * mode has no retained frame to transfer, but still completes the lease and
 * draws an active notification over the caller's viewport. */
bruce_result_t display__present(void);

/* Built-in launcher layout operation. Not exported to external runtimes. */
bruce_result_t display__set_tiles(const bruce_display_tile_t *tiles, size_t count);

/* -------------------------------------------------------------------------- */
/* Overlays                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * A generic always-on-top drawing surface any process may create for a menu,
 * notification, HUD, etc. Overlays are composed into every presented frame
 * (buffered mode: into transfer scratch rows; direct mode: painted right
 * after the underlying content) without ever touching another process's
 * framebuffer region or requiring the screen-wide lock, so drawing an
 * overlay never contends with whatever else is being drawn underneath it.
 *
 * Only the owning process may draw into, move, show, hide, or destroy its
 * own overlay. An overlay is automatically destroyed when its owning
 * process exits. There is no explicit z-order API in v1: overlays composite
 * in creation order, later-created on top of earlier ones.
 */

#define BRUCE_DISPLAY_MAX_OVERLAYS 8

typedef uint32_t bruce_display_overlay_id_t;
#define BRUCE_DISPLAY_OVERLAY_ID_INVALID ((bruce_display_overlay_id_t)0)

/* Creates a hidden overlay at `x, y` sized `w` x `h` (must fit on the
 * physical screen). Returns BRUCE_ERR_RESOURCE_LIMIT once
 * BRUCE_DISPLAY_MAX_OVERLAYS are live. */
bruce_result_t
display__overlay_create(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_overlay_id_t *out_overlay);

/* Destroys an overlay early. Also happens automatically on process exit. */
bruce_result_t display__overlay_destroy(bruce_display_overlay_id_t overlay);

/* Shows/hides an overlay without destroying it; content persists while
 * hidden. Both are idempotent. */
bruce_result_t display__overlay_show(bruce_display_overlay_id_t overlay);
bruce_result_t display__overlay_hide(bruce_display_overlay_id_t overlay);

/* Repositions an overlay's on-screen origin; size is fixed at creation. */
bruce_result_t display__overlay_move(bruce_display_overlay_id_t overlay, int16_t x, int16_t y);

/* Opens a drawing session on `overlay` for the calling (owning) process.
 * While open, every display__* draw call below (fill_rect, draw_string,
 * draw_bitmap, ...) plus display__width()/display__height() target the
 * overlay's own w x h buffer in overlay-local coordinates instead of the
 * caller's normal viewport. Sessions do not nest. */
bruce_result_t display__overlay_begin(bruce_display_overlay_id_t overlay);

/* Closes the drawing session opened by display__overlay_begin() and, if the
 * overlay is currently shown, repaints its rect. */
bruce_result_t display__overlay_end(bruce_display_overlay_id_t overlay);

#ifdef __cplusplus
}
#endif
