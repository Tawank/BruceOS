#pragma once

/**
 * @brief Screen drawing and display settings.
 *
 * This is the Core-side implementation of the JavaScript `display.*`
 * surface used by the bjs_interpreter. It is intentionally small and
 * immediate-mode: buffered drawing updates a Core-owned framebuffer and is
 * sent to the LCD when a frame is presented; direct drawing is streamed to
 * the panel immediately. GUI processes render in process-local coordinates
 * into a Core-assigned fullscreen or tiled viewport. Hidden processes see a
 * zero-sized viewport and drawing is a successful no-op.
 *
 * All coordinates are logical and respect the current rotation set by
 * display__set_rotation(). The framebuffer is stored in the logical
 * orientation for the current rotation; the ST7789 controller maps it to
 * the physical panel via esp_lcd_panel_swap_xy() / esp_lcd_panel_mirror().
 * The default rotation is set per board by
 * CONFIG_BRUCE_DISPLAY_DEFAULT_ROTATION (see src/Kconfig.projbuild and
 * boards/<board>/sdkconfig.defaults).
 *
 * Not permission-gated.
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

/** @brief Width of the caller's current viewport, or zero while hidden. */
int display__width(void);

/** @brief Height of the caller's current viewport, or zero while hidden. */
int display__height(void);

/**
 * @brief Full physical screen width, independent of the caller's own viewport.
 *
 * (which may be smaller, tiled, or zero while hidden/background). Meant for
 * positioning an overlay (see "Overlays" below): an overlay owner commonly
 * has no normal viewport of its own to measure against. Zero before
 * display__init().
 */
int display__screen_width(void);

/**
 * @brief Full physical screen height, independent of the caller's own viewport.
 *
 * (which may be smaller, tiled, or zero while hidden/background). Meant for
 * positioning an overlay (see "Overlays" below): an overlay owner commonly
 * has no normal viewport of its own to measure against. Zero before
 * display__init().
 */
int display__screen_height(void);

/**
 * @brief Convert 8-bit RGB components to an RGB565 color value.
 *
 * @param r Red component, 0-255.
 * @param g Green component, 0-255.
 * @param b Blue component, 0-255.
 */
bruce_display_color_t display__color565(uint8_t r, uint8_t g, uint8_t b);

/* -------------------------------------------------------------------------- */
/* Screen fill / clear                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill the entire logical screen with `color`.
 *
 * @param color Fill color.
 */
bruce_result_t display__fill_screen(bruce_display_color_t color);

/** @brief Fill the screen with the current background color (black by default). */
bruce_result_t display__clear(void);

/* -------------------------------------------------------------------------- */
/* Text state and output                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Set the foreground color used by subsequent text and bitmap drawing.
 *
 * @param color New foreground color.
 */
bruce_result_t display__set_text_color(bruce_display_color_t color);

/**
 * @brief Set the background color used for erasing behind text.
 *
 * Pass a value >= 0x10000 (e.g. BRUCE_COLOR_TRANSPARENT) for transparent
 * background rendering; otherwise the background rectangle is filled. The
 * parameter is 32-bit so the transparent sentinel fits.
 *
 * @param color New background color, or a value >= 0x10000 for transparent.
 */
bruce_result_t display__set_text_bg_color(uint32_t color);

/**
 * @brief Text size multiplier (1 = native character cells, 2 = 2x that, ...).
 *
 * Clamped to 0.1..8. Values below 1 downscale the native glyph bitmap
 * (nearest-pixel, not antialiased) instead of upscaling it -- since the
 * built-in font is only 5px wide to start with, legibility drops off fast
 * below ~0.5 and glyphs may fall back to a solid dot rather than a hollow
 * box at the smallest sizes. See display__get_font_metrics() for the native
 * cell size.
 *
 * @param size New text size multiplier (clamped to 0.1..8).
 */
bruce_result_t display__set_text_size(float size);

/**
 * @brief Position the text cursor in logical coordinates.
 *
 * @param x New cursor X position.
 * @param y New cursor Y position.
 */
bruce_result_t display__set_cursor(int16_t x, int16_t y);

/**
 * @brief Read the current text cursor position in logical coordinates.
 *
 * @param x Receives the cursor X position.
 * @param y Receives the cursor Y position.
 */
bruce_result_t display__get_cursor(int16_t *x, int16_t *y);

/**
 * @brief Print a NUL-terminated UTF-8 string at the cursor and advance the cursor.
 *
 * @param text Text to print.
 */
bruce_result_t display__print(const char *text);

/**
 * @brief Same as display__print() but appends a newline.
 *
 * @param text Text to print.
 */
bruce_result_t display__println(const char *text);

/**
 * @brief Draw a single-line string with (x, y) at its left edge.
 *
 * @param text Text to draw.
 * @param x Left edge X position.
 * @param y Y position.
 */
bruce_result_t display__draw_string(const char *text, int16_t x, int16_t y);

/**
 * @brief Draw a single-line string horizontally centered on x.
 *
 * @param text Text to draw.
 * @param x X position to center the text on.
 * @param y Y position.
 */
bruce_result_t display__draw_centre_string(const char *text, int16_t x, int16_t y);

/**
 * @brief Draw a single-line string with its right edge at x.
 *
 * @param text Text to draw.
 * @param x Right edge X position.
 * @param y Y position.
 */
bruce_result_t display__draw_right_string(const char *text, int16_t x, int16_t y);

/**
 * @brief The active font's fixed advance width/height, in px, before text_size scaling.
 *
 * (scale by the value passed to display__set_text_size() to get
 * on-screen pixels). Every built-in font is monospace today, so this
 * single width/height pair is exact for any string; callers doing their
 * own layout math (row heights, column counts, cursor placement) should
 * use this instead of a hardcoded guess, so they stay correct if the
 * active font ever changes.
 *
 * @param out_char_width Receives the character cell width in px.
 * @param out_char_height Receives the character cell height in px.
 */
bruce_result_t display__get_font_metrics(int16_t *out_char_width, int16_t *out_char_height);

/* -------------------------------------------------------------------------- */
/* Primitive drawing                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Draw a single pixel.
 *
 * @param x X position.
 * @param y Y position.
 * @param color Pixel color.
 */
bruce_result_t display__draw_pixel(int16_t x, int16_t y, bruce_display_color_t color);

/**
 * @brief Draw a line between two points.
 *
 * @param x0 First point's X position.
 * @param y0 First point's Y position.
 * @param x1 Second point's X position.
 * @param y1 Second point's Y position.
 * @param color Line color.
 */
bruce_result_t
display__draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bruce_display_color_t color);

/**
 * @brief Draw a rectangle outline.
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param w Width.
 * @param h Height.
 * @param color Outline color.
 */
bruce_result_t display__draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color);

/**
 * @brief Draw a filled rectangle.
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param w Width.
 * @param h Height.
 * @param color Fill color.
 */
bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color);

/**
 * @brief Draw a circle outline.
 *
 * @param x Center X position.
 * @param y Center Y position.
 * @param r Radius.
 * @param color Outline color.
 */
bruce_result_t display__draw_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color);

/**
 * @brief Draw a filled circle.
 *
 * @param x Center X position.
 * @param y Center Y position.
 * @param r Radius.
 * @param color Fill color.
 */
bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color);

/**
 * @brief Draw a circular arc.
 *
 * Angles use the legacy Bruce convention: zero degrees is at six o'clock
 * and values increase clockwise. An end angle less than the start angle
 * wraps through zero; a range of 360 degrees draws a full circle.
 *
 * @param x Center X position.
 * @param y Center Y position.
 * @param r Radius.
 * @param start_angle Start angle in degrees (0 = six o'clock, clockwise).
 * @param end_angle End angle in degrees.
 * @param color Arc color.
 */
bruce_result_t display__draw_arc(
    int16_t x, int16_t y, int16_t r, int16_t start_angle, int16_t end_angle, bruce_display_color_t color
);

/**
 * @brief Draw a triangle outline.
 *
 * @param x0 First vertex X position.
 * @param y0 First vertex Y position.
 * @param x1 Second vertex X position.
 * @param y1 Second vertex Y position.
 * @param x2 Third vertex X position.
 * @param y2 Third vertex Y position.
 * @param color Outline color.
 */
bruce_result_t display__draw_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
);

/**
 * @brief Draw a filled triangle.
 *
 * @param x0 First vertex X position.
 * @param y0 First vertex Y position.
 * @param x1 Second vertex X position.
 * @param y1 Second vertex Y position.
 * @param x2 Third vertex X position.
 * @param y2 Third vertex Y position.
 * @param color Fill color.
 */
bruce_result_t display__fill_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
);

/**
 * @brief Draw a rounded-rectangle outline.
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param w Width.
 * @param h Height.
 * @param r Corner radius.
 * @param color Outline color.
 */
bruce_result_t
display__draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color);

/**
 * @brief Draw a filled rounded rectangle.
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param w Width.
 * @param h Height.
 * @param r Corner radius.
 * @param color Fill color.
 */
bruce_result_t
display__fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color);

/* -------------------------------------------------------------------------- */
/* Bitmaps                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Draw a 1bpp MSB-first bitmap.
 *
 * Each row is byte-aligned; a set bit draws `color`, a clear bit draws the
 * current background color (or is skipped when the background is
 * transparent).
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param bitmap 1bpp MSB-first, byte-aligned-per-row bitmap data.
 * @param w Bitmap width in pixels.
 * @param h Bitmap height in pixels.
 * @param color Color drawn for each set bit.
 */
bruce_result_t display__draw_bitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
);

/**
 * @brief Draw a 1bpp XBM bitmap.
 *
 * Each row is byte-aligned; a set bit draws `color`, a clear bit is
 * transparent.
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param bitmap 1bpp XBM, byte-aligned-per-row bitmap data.
 * @param w Bitmap width in pixels.
 * @param h Bitmap height in pixels.
 * @param color Color drawn for each set bit.
 */
bruce_result_t display__draw_xbitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
);

/**
 * @brief Draw a raw RGB565 bitmap from `bitmap` (width * height * 2 bytes).
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param bitmap Raw RGB565 pixel data, width * height * 2 bytes.
 * @param w Bitmap width in pixels.
 * @param h Bitmap height in pixels.
 */
bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);

/**
 * @brief Draw a 1bpp MSB-first bitmap of `w` x `h` pixels scaled to `dw` x `dh`.
 *
 * Using nearest-neighbor sampling. A set bit draws `color`; a clear bit is
 * transparent. This is the fast path for built-in icons (see icon.h): pure
 * integer math, no parsing, no extra RAM.
 *
 * @param x Left edge X position.
 * @param y Top edge Y position.
 * @param bitmap 1bpp MSB-first, byte-aligned-per-row bitmap data.
 * @param w Source bitmap width in pixels.
 * @param h Source bitmap height in pixels.
 * @param dw Destination (scaled) width in pixels.
 * @param dh Destination (scaled) height in pixels.
 * @param color Color drawn for each set bit.
 */
bruce_result_t display__draw_bitmap_scaled(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, int16_t dw, int16_t dh,
    bruce_display_color_t color
);

/* -------------------------------------------------------------------------- */
/* Display control                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Set rotation: 0 = native, 1 = 90deg clockwise, 2 = 180, 3 = 270.
 *
 * @param rotation New rotation (0..3).
 */
bruce_result_t display__set_rotation(uint8_t rotation);

/** @brief Return the current rotation (0..3). */
uint8_t display__get_rotation(void);

/**
 * @brief Copies the composed RGB565 framebuffer for remote display tools.
 *
 * Pass NULL with capacity 0 to query dimensions and required pixel count.
 * Returns BRUCE_ERR_UNSUPPORTED when buffered rendering is disabled.
 *
 * @param pixels Buffer to receive RGB565 pixel data, or NULL to only query dimensions.
 * @param capacity Number of pixels the pixels buffer can hold.
 * @param out_width Receives the framebuffer width in pixels.
 * @param out_height Receives the framebuffer height in pixels.
 * @param out_pixel_count Receives the total pixel count required.
 */
bruce_result_t display__snapshot(
    uint16_t *pixels,
    size_t capacity,
    uint16_t *out_width,
    uint16_t *out_height,
    size_t *out_pixel_count
);

/**
 * @brief Invert the panel colors (independent of color_inverted config).
 *
 * @param invert True to invert, false to restore normal colors.
 */
bruce_result_t display__invert_display(bool invert);

/**
 * @brief Set backlight brightness, 0..255. Persists via config__set_display_brightness().
 *
 * @param brightness New backlight brightness (0..255).
 */
bruce_result_t display__set_brightness(uint8_t brightness);

/** @brief Return the current backlight brightness, 0..255. */
uint8_t display__get_brightness(void);

/**
 * @brief Turn the panel display on or off (backlight is controlled separately).
 *
 * @param on True to turn the panel on, false to turn it off.
 */
bruce_result_t display__display_on_off(bool on);

/**
 * @brief Turns direct game rendering on or off.
 *
 * Entirely at runtime, no reboot required. Meant to be called by a
 * memory-hungry ELF/JS app (an emulator, a game with a large asset set,
 * ...) right before it starts allocating, to hand back the RAM the
 * buffered/DMA framebuffer would otherwise be holding for the whole time
 * the app runs.
 *
 * display__game_mode(true) tears down the current s_framebuffer/pack buffer
 * (if buffered rendering was in use) and allocates only the small
 * direct-mode scratch buffer instead; the calling process becomes game
 * mode's owner. display__game_mode(false), called by that same owning
 * process, restores whatever rendering mode was active before game mode
 * was turned on. Calling it again with the same value the caller already
 * holds is a no-op. Only the owning process may turn its own game mode
 * back off; if the owner exits (or is killed) without doing so, the
 * display core reverts it automatically.
 *
 * Fails with BRUCE_ERR_BUSY if another process currently owns game mode,
 * or while any process has an active (begun but not yet presented) frame.
 *
 * @param enable True to enter game mode, false to revert to the prior rendering mode.
 */
bruce_result_t display__game_mode(bool enable);

/* -------------------------------------------------------------------------- */
/* Framebuffer flush                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Lease the caller's current viewport for one complete frame.
 *
 * In direct mode, drawing is visible before the frame is presented.
 */
bruce_result_t display__begin_frame(void);

/**
 * @brief Present an active frame and wait until its LCD transfer completes.
 *
 * Direct mode has no retained frame to transfer, but still completes the
 * lease and draws an active notification over the caller's viewport.
 */
bruce_result_t display__present(void);

/**
 * @brief Built-in launcher layout operation. Not exported to external runtimes.
 *
 * @param tiles New tile layout (one rect per process).
 * @param count Number of entries in tiles.
 */
bruce_result_t display__set_tiles(const bruce_display_tile_t *tiles, size_t count);

/* -------------------------------------------------------------------------- */
/* Overlays                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Overlays: a generic always-on-top drawing surface any process may create.
 *
 * For a menu, notification, HUD, etc. Overlays are composed into every
 * presented frame (buffered mode: into transfer scratch rows; direct mode:
 * painted right after the underlying content) without ever touching
 * another process's framebuffer region or requiring the screen-wide lock,
 * so drawing an overlay never contends with whatever else is being drawn
 * underneath it.
 *
 * Only the owning process may draw into, move, show, hide, or destroy its
 * own overlay. An overlay is automatically destroyed when its owning
 * process exits. There is no explicit z-order API in v1: overlays
 * composite in creation order, later-created on top of earlier ones.
 */

#define BRUCE_DISPLAY_MAX_OVERLAYS 8

typedef uint32_t bruce_display_overlay_id_t;
#define BRUCE_DISPLAY_OVERLAY_ID_INVALID ((bruce_display_overlay_id_t)0)

/**
 * @brief Creates a hidden overlay at `x, y` sized `w` x `h`.
 *
 * Must fit on the physical screen. Returns BRUCE_ERR_RESOURCE_LIMIT once
 * BRUCE_DISPLAY_MAX_OVERLAYS are live.
 *
 * @param x Overlay origin X position.
 * @param y Overlay origin Y position.
 * @param w Overlay width.
 * @param h Overlay height.
 * @param out_overlay Receives the new overlay's id.
 */
bruce_result_t
display__overlay_create(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_overlay_id_t *out_overlay);

/**
 * @brief Destroys an overlay early. Also happens automatically on process exit.
 *
 * @param overlay Overlay to destroy.
 */
bruce_result_t display__overlay_destroy(bruce_display_overlay_id_t overlay);

/**
 * @brief Shows an overlay without destroying it.
 *
 * Content persists while hidden. Idempotent.
 *
 * @param overlay Overlay to show.
 */
bruce_result_t display__overlay_show(bruce_display_overlay_id_t overlay);

/**
 * @brief Hides an overlay without destroying it.
 *
 * Content persists while hidden. Idempotent.
 *
 * @param overlay Overlay to hide.
 */
bruce_result_t display__overlay_hide(bruce_display_overlay_id_t overlay);

/**
 * @brief Repositions an overlay's on-screen origin; size is fixed at creation.
 *
 * @param overlay Overlay to move.
 * @param x New origin X position.
 * @param y New origin Y position.
 */
bruce_result_t display__overlay_move(bruce_display_overlay_id_t overlay, int16_t x, int16_t y);

/**
 * @brief Opens a drawing session on `overlay` for the calling (owning) process.
 *
 * While open, every display__* draw call below (fill_rect, draw_string,
 * draw_bitmap, ...) plus display__width()/display__height() target the
 * overlay's own w x h buffer in overlay-local coordinates instead of the
 * caller's normal viewport. Sessions do not nest.
 *
 * @param overlay Overlay to begin drawing into.
 */
bruce_result_t display__overlay_begin(bruce_display_overlay_id_t overlay);

/**
 * @brief Closes the drawing session opened by display__overlay_begin().
 *
 * If the overlay is currently shown, repaints its rect.
 *
 * @param overlay Overlay whose drawing session to close.
 */
bruce_result_t display__overlay_end(bruce_display_overlay_id_t overlay);

#ifdef __cplusplus
}
#endif
