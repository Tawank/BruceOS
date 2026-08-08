#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/display.h"
#include "core_sdk/runtime.h"

#include "bitmap.h"
#include "nes/nes.h"
#include "nes_sound.h"
#include "nes_video.h"
#include "nofrendo.h"
#include "osd.h"

static uint8_t s_framebuffer_dummy;
static bitmap_t *s_screen_bitmap;
static uint16_t s_palette[256];
static void (*s_timer_callback)(void);

void nes_video_install_timer(void (*callback)(void)) { s_timer_callback = callback; }

static int video_init(int width, int height) {
    (void)width;
    (void)height;
    return 0;
}

static void video_shutdown(void) {}

static int video_set_mode(int width, int height) {
    (void)width;
    (void)height;
    return 0;
}

static void video_set_palette(rgb_t *colors) {
    for (int i = 0; i < 256; ++i) { s_palette[i] = display__color565(colors[i].r, colors[i].g, colors[i].b); }
}

static void video_clear(uint8 color) {
    (void)color;
    if (display__begin_frame() == BRUCE_OK) {
        display__fill_screen(BRUCE_COLOR_BLACK);
        display__present();
    }
}

static bitmap_t *video_lock(void) {
    if (s_screen_bitmap == NULL) {
        s_screen_bitmap =
            bmp_createhw(&s_framebuffer_dummy, NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, NES_SCREEN_WIDTH);
    }
    return s_screen_bitmap;
}

static void video_free(int num_dirties, rect_t *dirty_rects) {
    (void)num_dirties;
    (void)dirty_rects;
}

void nes_video_destroy_bitmap(void) { bmp_destroy(&s_screen_bitmap); }

/* `1000 / NES_REFRESH_RATE` (60 or 50) truncates to a whole millisecond --
 * 16ms for NTSC instead of the real NES's ~16.639ms (its actual refresh rate
 * is 60.0988Hz, not exactly 60Hz). That truncation was silently pacing ahead
 * whenever this task had idle time to spend. Track the sub-millisecond
 * remainder (~0.639ms/frame for NTSC) in a separate counter so it survives
 * from one frame to the next instead of being dropped, and use the real NES
 * refresh rate rather than the rounded one.
 *
 * This deliberately avoids ever dividing/moduloing a uint64_t at runtime:
 * the ELF loader only resolves against a fixed SDK symbol table (see
 * elf_loader_sdk_symbols.c), which exports the float/double libgcc helpers
 * but not the 64-bit integer ones (__udivdi3/__umoddi3) -- Xtensa has no
 * hardware divider and GCC always calls out to those for a 64-bit divide, so
 * using one here fails to relocate at load time instead of failing to build.
 * NES_FRAME_PERIOD_US/1000u and %1000u below are both compile-time constants
 * that fold to literals, so no division instruction is ever emitted for
 * them; the only per-frame math is 64-bit add/compare/subtract. */
#if NES_REFRESH_RATE == 50
#define NES_FRAME_PERIOD_US 20000u /* PAL: 50Hz */
#else
#define NES_FRAME_PERIOD_US 16639u /* NTSC: real refresh rate is 60.0988Hz */
#endif

/* A single pace_frame() call used to bump nofrendo_ticks by a flat 1 no
 * matter how long the frame it just finished actually took in real time.
 * That was harmless while autoframeskip was permanently forced off (see
 * nes_osd.c's osd_installtimer()) -- with autoframeskip off nes_emulate()
 * renders every call regardless of ticks, so the tick value was never
 * actually load-bearing. It matters now: nofrendo's own frame-skip logic
 * decides whether to render pixels this frame purely by comparing elapsed
 * ticks against frames actually drawn, and a flat +1 told it "exactly one
 * frame period passed" even when the previous render+blit took two or three
 * times that long. nofrendo then dutifully rendered and blitted every single
 * one of those slow frames in full, so the *whole game* -- not just the
 * frame rate -- ran in slow motion, and nes_sound_pump() (also only called
 * from here) could never pull more than one frame's worth of audio per call
 * to match, so the output ring chronically underran into distortion. Ticks
 * bumped here now reflect how many real NES_FRAME_PERIOD_US periods actually
 * elapsed, so nes_emulate() can tell it's behind and skip pixel rendering
 * (nes_renderframe(false), still real CPU/PPU emulation, just no draw) on
 * some frames to let game logic catch back up to real time -- see
 * osd_installtimer() for how the very first frame bootstraps this. */
#define NES_MAX_TICKS_PER_PACE 6u

static void pace_frame(void) {
    static uint64_t next_frame_ms;
    static uint32_t next_frame_remainder_us;
    static bool autoframeskip_restored;

    uint64_t now_ms = runtime__now();
    if (next_frame_ms == 0) next_frame_ms = now_ms;

    uint32_t ticks = 0;
    while (next_frame_ms <= now_ms && ticks < NES_MAX_TICKS_PER_PACE) {
        next_frame_ms += NES_FRAME_PERIOD_US / 1000u;
        next_frame_remainder_us += NES_FRAME_PERIOD_US % 1000u;
        if (next_frame_remainder_us >= 1000u) {
            next_frame_remainder_us -= 1000u;
            next_frame_ms += 1;
        }
        ticks++;
    }
    if (ticks == 0) {
        /* Ahead of schedule: still owe the one period this frame covers. */
        next_frame_ms += NES_FRAME_PERIOD_US / 1000u;
        next_frame_remainder_us += NES_FRAME_PERIOD_US % 1000u;
        if (next_frame_remainder_us >= 1000u) {
            next_frame_remainder_us -= 1000u;
            next_frame_ms += 1;
        }
        ticks = 1;
    } else if (ticks >= NES_MAX_TICKS_PER_PACE && next_frame_ms <= now_ms) {
        /* Behind by more than NES_MAX_TICKS_PER_PACE periods (backgrounded,
         * a long stall, ...): fully replaying the missed time would itself
         * look like a multi-frame freeze once caught up. Resume pacing from
         * now instead, the same way nes_sound.c's BRUCE_NES_AUDIO_MAX_GAP_MS
         * caps its own catch-up. */
        next_frame_ms = now_ms + NES_FRAME_PERIOD_US / 1000u;
        next_frame_remainder_us = NES_FRAME_PERIOD_US % 1000u;
    }

    if (next_frame_ms > now_ms) {
        runtime__delay((uint32_t)(next_frame_ms - now_ms));
    } else {
        /* Emulation + blit ran over budget: there is no actual idle time to
         * spend, but this task must still hit a real scheduling point every
         * frame. Without it, once emulation can't keep up with
         * NES_REFRESH_RATE this branch runs every frame forever and the task
         * never blocks -- starving IDLE0 (this task is CPU0-pinned) and
         * tripping the task watchdog. A 1ms runtime__delay() always blocks
         * for at least one tick (see process__wait_ms()), so this yields the
         * core each frame while only slightly slowing an already-behind
         * emulator. */
        runtime__delay(1);
    }

    for (uint32_t i = 0; i < ticks; i++) {
        if (s_timer_callback != NULL) s_timer_callback();
    }

    if (!autoframeskip_restored) {
        /* osd_installtimer() forced autoframeskip off for exactly the
         * bootstrap frame nofrendo renders before nes_emulate() has ever
         * observed a tick change. A real tick has now been bumped above and
         * nes_emulate()'s frames_to_render bookkeeping is live, so hand
         * control back to nofrendo's own autoframeskip (its nes_create()
         * default) for every frame from here on. */
        autoframeskip_restored = true;
        nes_t *machine = nes_getcontextptr();
        if (machine != NULL) machine->autoframeskip = true;
    }

    nes_sound_pump();
}

#define BRUCE_NES_VISIBLE_HEIGHT 224
#define BRUCE_NES_BLIT_MAX_WIDTH 320
/* Rows batched into one display__draw_rgb_bitmap() call. Blitting one
 * scanline at a time (h=1) turned every frame into 200+ separate DMA
 * transactions, each paying its own address-window/ISR round-trip and each
 * becoming visible on the physical panel the instant it landed -- the
 * visible top-to-bottom "drawing itself" effect. Batching cuts that to a
 * handful of transfers per frame; the buffer below is intentionally small
 * (a few KB) rather than a full offscreen frame. */
#define BRUCE_NES_BLIT_ROWS 8
static uint16_t s_scaled_lines[BRUCE_NES_BLIT_ROWS * BRUCE_NES_BLIT_MAX_WIDTH];

/* Last geometry the letterbox borders were cleared for -- see video_blit()'s
 * border clear below. Impossible values force a clear on the first frame. */
static int s_last_origin_x = -1;
static int s_last_origin_y = -1;
static int s_last_draw_width = -1;
static int s_last_draw_height = -1;
static int s_last_screen_width = -1;
static int s_last_screen_height = -1;

static void video_blit(bitmap_t *bitmap, int num_dirties, rect_t *dirty_rects) {
    (void)num_dirties;
    (void)dirty_rects;

    int screen_width = display__width();
    int screen_height = display__height();
    if (screen_width <= 0 || screen_height <= 0) {
        pace_frame();
        return;
    }

    const int source_height = BRUCE_NES_VISIBLE_HEIGHT;
    const int source_y = (NES_SCREEN_HEIGHT - source_height) / 2;

    /* Horizontal axis is never scaled. Nearest-neighbor resampling with a
     * non-integer step (the old `draw_width * NES_SCREEN_WIDTH / draw_width`
     * ratio) maps source columns to destination pixels with a fractional
     * accumulator that rolls over at a different destination pixel every
     * frame as the emulator's internal scroll position changes -- so a
     * horizontally scrolling background doesn't just shift a constant number
     * of source pixels per frame, some columns get duplicated or dropped
     * unevenly, seen as a horizontal "bobbing"/wobble riding on top of the
     * scroll. Keeping this axis 1:1 means destination pixel x always maps to
     * source column x, every frame, so scrolling is a plain pixel shift with
     * no wobble. Crop the sides if the screen is narrower than the NES's
     * 256px width, or center it with black margins if wider. */
    int draw_width = NES_SCREEN_WIDTH;
    if (draw_width > screen_width) draw_width = screen_width;
    if (draw_width > BRUCE_NES_BLIT_MAX_WIDTH) { draw_width = BRUCE_NES_BLIT_MAX_WIDTH; }
    int source_x = (NES_SCREEN_WIDTH - draw_width) / 2;

    /* Vertical axis still scales (squashes/stretches) to fill the screen
     * height exactly -- it isn't tied to horizontal scroll position so it
     * doesn't exhibit the same wobble. */
    int draw_height = screen_height;
    int origin_x = (screen_width - draw_width) / 2;
    int origin_y = (screen_height - draw_height) / 2;

    if (display__begin_frame() == BRUCE_OK) {
        /* The NES image covers [origin_x, origin_y, draw_width, draw_height]
         * every frame, so only the letterbox borders around it ever need
         * clearing -- and only once, when that geometry first appears or
         * changes, not on every redraw (painting the whole screen black
         * before every frame was the dominant source of visible flicker). */
        if (origin_x != s_last_origin_x || origin_y != s_last_origin_y || draw_width != s_last_draw_width ||
            draw_height != s_last_draw_height || screen_width != s_last_screen_width ||
            screen_height != s_last_screen_height) {
            if (origin_y > 0) display__fill_rect(0, 0, screen_width, origin_y, BRUCE_COLOR_BLACK);
            if (origin_y + draw_height < screen_height) {
                display__fill_rect(
                    0,
                    origin_y + draw_height,
                    screen_width,
                    screen_height - (origin_y + draw_height),
                    BRUCE_COLOR_BLACK
                );
            }
            if (origin_x > 0) display__fill_rect(0, origin_y, origin_x, draw_height, BRUCE_COLOR_BLACK);
            if (origin_x + draw_width < screen_width) {
                display__fill_rect(
                    origin_x + draw_width,
                    origin_y,
                    screen_width - (origin_x + draw_width),
                    draw_height,
                    BRUCE_COLOR_BLACK
                );
            }
            s_last_origin_x = origin_x;
            s_last_origin_y = origin_y;
            s_last_draw_width = draw_width;
            s_last_draw_height = draw_height;
            s_last_screen_width = screen_width;
            s_last_screen_height = screen_height;
        }

        /* Vertical nearest-neighbor scale factor as 16.16 fixed point. The
         * Xtensa cores on this board have no hardware integer divider, so
         * computing this ratio once per frame and walking it with an add +
         * shift per row (rather than dividing per pixel) keeps this out of
         * the ~90000-iteration-per-frame pixel loop below. The horizontal
         * axis is no longer scaled at all -- see the draw_width/source_x
         * comment above -- so the inner loop is a direct copy offset by
         * source_x. */
        uint32_t y_step = ((uint32_t)source_height << 16) / (uint32_t)draw_height;

        for (int y = 0; y < draw_height; y += BRUCE_NES_BLIT_ROWS) {
            int rows = draw_height - y;
            if (rows > BRUCE_NES_BLIT_ROWS) rows = BRUCE_NES_BLIT_ROWS;
            for (int row = 0; row < rows; ++row) {
                int src_row = source_y + (((uint32_t)(y + row) * y_step) >> 16);
                const uint8_t *source = bitmap->line[src_row] + source_x;
                uint16_t *dst = &s_scaled_lines[row * draw_width];
                for (int x = 0; x < draw_width; ++x) { dst[x] = s_palette[source[x]]; }
            }
            display__draw_rgb_bitmap(origin_x, origin_y + y, s_scaled_lines, draw_width, rows);
            /* Pump audio between row batches too, not just once via
             * pace_frame() after the whole blit (and its own
             * display__present() wait) completes. nes_sound_pump() sizes
             * each pull from real elapsed time and is cheap to call with
             * little or nothing due yet, so this just spreads the same total
             * catch-up across smaller, more frequent pulls instead of one
             * lump sized by however long this entire frame's DMA transfers
             * take -- keeping any one pull well under
             * BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME even on a frame slow
             * enough to need several row batches. */
            nes_sound_pump();
        }
        display__present();
    }
    pace_frame();
}

static viddriver_t bruce_video_driver = {
    "Bruce RGB565",
    video_init,
    video_shutdown,
    video_set_mode,
    video_set_palette,
    video_clear,
    video_lock,
    video_free,
    video_blit,
    false,
};

void osd_getvideoinfo(vidinfo_t *info) {
    info->default_width = NES_SCREEN_WIDTH;
    info->default_height = NES_SCREEN_HEIGHT;
    info->driver = &bruce_video_driver;
}
