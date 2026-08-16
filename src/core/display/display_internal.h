#pragma once

#include "core_sdk/display.h"
#include "core_sdk/process.h"
#include "display_font.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"

#define DISPLAY__NATIVE_WIDTH CONFIG_BRUCE_DISPLAY_WIDTH
#define DISPLAY__NATIVE_HEIGHT CONFIG_BRUCE_DISPLAY_HEIGHT
#define DISPLAY__FB_SIZE (DISPLAY__NATIVE_WIDTH * DISPLAY__NATIVE_HEIGHT * sizeof(bruce_display_color_t))
#define DISPLAY__DIRECT_BUF_PIXELS                                                                        \
    (DISPLAY__NATIVE_HEIGHT * ((DISPLAY__NATIVE_WIDTH + 3) / 4))
/* s_direct_buffer (display.c) is split into two chunks of this size -- a
 * ping-pong pair reusing the same total allocation as before, not extra RAM.
 * display_internal__draw_rgb_bitmap() packs the next chunk's pixels into
 * whichever half isn't currently mid-DMA-transfer, so that packing overlaps
 * the previous chunk's transfer instead of happening strictly after it. */
#define DISPLAY__DIRECT_CHUNK_PIXELS (DISPLAY__DIRECT_BUF_PIXELS / 2)

/* Nominal cell of the *active* font (display_internal__active_font()), used
 * by the small set of callers still asking for it in whole-pixel terms
 * (dialog/text/terminal layout, notification sizing). DISPLAY__FONT_HEIGHT
 * is a max row index (real height is DISPLAY__FONT_HEIGHT + 1): rows 0-1 are
 * reserved so accented glyphs (display_font_bitmap.c) have real headroom for
 * a diacritic instead of overlapping the letter's own ink -- see
 * display_font.h for why that matters and how a second font provider (e.g.
 * a future FreeType-backed one) would plug in. */
#define DISPLAY__FONT_WIDTH 5
#define DISPLAY__FONT_HEIGHT 9
#define DISPLAY__FONT_FIRST 32
#define DISPLAY__FONT_LAST 126

#define DISPLAY__MAX_OVERLAYS BRUCE_DISPLAY_MAX_OVERLAYS

struct display__overlay_s;

/* A drawing surface: either a process's own screen viewport (drawing goes
 * into the shared s_framebuffer / direct panel path, selected by
 * target_buffer == NULL) or an overlay's private buffer (target_buffer !=
 * NULL, always written directly regardless of the main display's
 * buffered/direct mode). Every display__* draw primitive resolves one of
 * these via display_internal__begin_draw() and writes only into it, so two
 * contexts never touch each other's memory. `lock` serializes drawing into
 * this one surface only -- it is never held across two different contexts,
 * which is what lets unrelated processes/tiles/overlays draw concurrently. */
typedef struct {
    bool in_use;
    bool gui_requested;
    bool built_in;
    bool tiled;
    bool hidden;
    bool clear_on_next_frame;
    bool frame_active;
    bool frame_noop;
    bruce_process_id_t process_id;
    bruce_process_state_t state;
    bruce_display_rect_t viewport;
    uint32_t viewport_generation;
    uint32_t frame_generation;
    bruce_result_t draw_result;
    bruce_display_color_t text_color;
    bruce_display_color_t text_bg_color;
    bool text_bg_transparent;
    uint8_t text_size;
    int16_t cursor_x;
    int16_t cursor_y;

    /* Per-surface drawing lock; held only for the duration of one draw
     * primitive on this context (see display_internal__begin_draw()).
     * Created once, lazily, and never destroyed -- slots are reused across
     * process lifetimes like the rest of this table. */
    SemaphoreHandle_t lock;

    /* Non-NULL for a normal process context whose calling process has an
     * overlay drawing session open (see display__overlay_begin()): draw
     * calls resolve to &active_overlay->surface instead of this context.
     * Only ever written by the owning process's own task. Always NULL on an
     * overlay's own `surface` context. */
    struct display__overlay_s *active_overlay;

    /* NULL selects the shared s_framebuffer/direct-panel path at
     * (viewport.x + x, viewport.y + y), clipped to the physical screen.
     * Non-NULL (an overlay's private pixel buffer) is written at local (x,
     * y) with this stride, clipped to viewport.width/height only -- set on
     * an overlay's `surface` context, never on a normal process context. */
    bruce_display_color_t *target_buffer;
    int16_t target_stride;
} display__process_context_t;

/* A generic on-top-of-everything drawing layer any process can create for
 * menus, notifications, HUDs, etc. Composed into every display__flush()
 * (buffered mode: into transfer scratch rows; direct mode: painted after
 * the caller's own content) without ever touching another process's
 * framebuffer region, so it needs no coordination with the owner of the
 * screen area underneath it. `id` is a monotonically increasing,
 * process-lifetime-unique value (never reused) so a stale handle can never
 * alias a later overlay reusing the same pool slot. */
typedef struct display__overlay_s {
    bool in_use;
    uint32_t id;
    uint32_t generation; /* bumped on show/hide/move/end-of-draw */
    bruce_process_id_t owner;
    bruce_resource_id_t resource_id;
    bool visible;
    bruce_display_rect_t rect;      /* on-screen position and size */
    bruce_display_color_t *pixels;  /* rect.width * rect.height, private */
    display__process_context_t surface; /* cursor/text state + draw target */
} display__overlay_t;

bruce_result_t display_internal__begin_draw(display__process_context_t **context);
void display_internal__unlock(display__process_context_t *context);
void display_internal__set_pixel(display__process_context_t *context, int16_t x, int16_t y, bruce_display_color_t color);
void display_internal__fill_rect(
    display__process_context_t *context, int16_t x, int16_t y, int16_t w, int16_t h,
    bruce_display_color_t color
);
void display_internal__draw_rgb_bitmap(
    display__process_context_t *context, int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h
);
bool display_internal__on_transfer_done_from_isr(void);

/* The font every display__* text primitive currently renders through.
 * Defaults to display_font_bitmap__instance(); swappable at runtime so a
 * second provider can be added later (see display_font.h) without changing
 * display_text.c's rendering loop or its core_sdk callers. No core_sdk API
 * exposes the setter yet -- add one only once a second provider exists and
 * needs to be user-selectable. */
void display_internal__set_font(const display__font_t *font);
const display__font_t *display_internal__active_font(void);

/* -------------------------------------------------------------------------- */
/* Overlay pool (display_overlay.c): owns s_overlays[], exposes pure         */
/* geometry/compositing helpers to display.c, which still owns the transfer  */
/* buffers and DMA sequencing.                                               */
/* -------------------------------------------------------------------------- */

void display_overlay__deinit(void);

/* -------------------------------------------------------------------------- */
/* display.c services exposed to display_overlay.c. The registry lock is the */
/* single short-critical-section lock shared by both files for structural    */
/* state (context table, overlay table, viewport/visibility, frame leases).  */
/* Per-surface `lock` fields (see display__process_context_t/overlay above)  */
/* are never touched through this lock.                                      */
/* -------------------------------------------------------------------------- */

void display_internal__lock_registry(void);
void display_internal__unlock_registry(void);
bool display_internal__initialized(void);
void display_internal__screen_size(int16_t *out_width, int16_t *out_height);
/* Caller must hold the registry lock. Returns the system context for
 * BRUCE_PROCESS_ID_INVALID. */
display__process_context_t *display_internal__find_context_locked(bruce_process_id_t process_id);
/* Smallest rect enclosing both `a` and `b`; either may be zero-sized. */
bruce_display_rect_t display_internal__rect_union(bruce_display_rect_t a, bruce_display_rect_t b);
/* Repaints `rect` immediately (composing any overlay over it) unless an
 * active frame currently owns that screen area, in which case the overlay
 * simply appears on that frame's next transfer. Caller must hold the
 * registry lock; `rect` must already be clipped to the screen. */
bruce_result_t display_internal__repaint_rect_locked(bruce_display_rect_t rect);

/* Streams one contiguous, already-composited row of `width` pixels straight
 * to the panel at (x, y), waiting for the DMA transfer to complete before
 * returning. Direct mode only; a no-op returning BRUCE_OK under
 * CONFIG_BRUCE_QEMU_TEST_MODE. Caller must hold the registry lock (direct
 * mode drawing already runs lock-held end to end, unlike the buffered
 * transfer path). `width` must not exceed the shared row scratch buffer. */
bruce_result_t
display_internal__stream_row_locked(int16_t x, int16_t y, int16_t width, const bruce_display_color_t *pixels);

/* Overlays are opaque rectangles: every pixel in an overlay's rect is drawn
 * over whatever is underneath, in creation order (later overlays on top).
 * There is no per-pixel transparency key -- an owner that wants a border or
 * background paints it itself, exactly like a normal viewport. */

/* True if any visible overlay intersects `rect`. Used by display.c to decide
 * whether a transfer needs row-by-row composition. Caller must hold the
 * display registry lock. */
bool display_overlay__intersects_locked(bruce_display_rect_t rect);

/* Buffered-mode compositing: overwrites the pixels of `row_buffer` (indexed
 * from `transfer.x`, already seeded with the real framebuffer row by the
 * caller) that fall within a visible overlay's rect at `screen_y`. Caller
 * must hold the display registry lock. */
void display_overlay__compose_row_locked(
    bruce_display_rect_t transfer, int screen_y, bruce_display_color_t *row_buffer
);

/* Direct-mode compositing: streams every visible overlay's own rows that
 * intersect `clip` straight to the panel via display_internal__stream_row_locked(),
 * in creation order. There is no shadow framebuffer in direct mode, so
 * (unlike the buffered path) nothing outside an overlay's own rect is
 * touched. Caller must hold the display registry lock. */
bruce_result_t display_overlay__paint_direct_locked(bruce_display_rect_t clip);

/* Selftest introspection seam. */
bruce_result_t display_overlay__test_state(
    bruce_display_overlay_id_t overlay, bruce_display_rect_t *out_rect, bool *out_visible,
    uint32_t *out_generation
);
/* Reads one pixel from `overlay`'s own private buffer at local (x, y),
 * regardless of who owns it or whether the overlay is currently visible.
 * Selftest introspection seam. */
bruce_result_t display_overlay__test_pixel(
    bruce_display_overlay_id_t overlay, int16_t x, int16_t y, bruce_display_color_t *out_color
);
