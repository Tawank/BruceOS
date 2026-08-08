#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* --- Bottleneck measurement + display-only frame skip ---------------------
 *
 * Every video_blit() call is one full loop iteration: nofrendo has already
 * finished emulating CPU+PPU for this frame by the time this callback runs
 * (nes_emulate() in nes/nes.c calls nes_renderframe() unconditionally before
 * blitting, see osd_installtimer()'s comment in nes_osd.c for why
 * autoframeskip can't be used to skip that part on this port), so the only
 * cost this OSD layer can shed here is its own palette conversion + DMA
 * blit below. Two independent things are tracked from the same wall-clock
 * reading:
 *
 *  1. Rolling perf counters (s_perf_*), reported over printf periodically,
 *     splitting each iteration's real time into "everything before this
 *     callback" (CPU/PPU emulation, input, scheduling) and "blit" (this
 *     file's own scale+DMA work) -- the actual bottleneck breakdown, to
 *     measure instead of guess where frame time is going on real hardware.
 *  2. s_skip_debt_us, how far behind the NES_FRAME_PERIOD_US budget the last
 *     few iterations have run. Once a full frame period behind, this frame's
 *     blit is skipped entirely (pace_frame() -- and therefore audio+input --
 *     still runs as normal) so the next iteration starts sooner instead of
 *     drawing a frame that's already stale and falling further behind. This
 *     never touches nofrendo_ticks/autoframeskip/nes_renderframe -- it only
 *     ever removes this file's own display work, nothing upstream of it. */
#define NES_PERF_REPORT_INTERVAL_MS 2000u
/* A gap at least this long is a real stall (ROM just loaded, a dialog was
 * open, the process was backgrounded) rather than ordinary lateness -- start
 * both the debt and the perf averages fresh instead of letting one huge
 * sample dominate them or trigger a long run of skipped frames afterward. */
#define NES_PERF_STALL_MS 500u
/* Caps how many frame-periods of debt can build up, in turn capping how many
 * *consecutive* frames get skipped once things are bad enough to trigger
 * skipping at all -- bounds the worst case to "every other frame" rather
 * than a long unbroken run of blank-looking frames. */
#define NES_SKIP_MAX_DEBT_US (NES_FRAME_PERIOD_US * 3u)

static uint64_t s_last_iter_ms;
static uint32_t s_skip_debt_us;

static uint64_t s_perf_report_ms;
static uint32_t s_perf_frames_drawn;
static uint32_t s_perf_frames_skipped;
/* Plain uint32_t, not uint64_t: NES_PERF_REPORT_INTERVAL_MS bounds how long
 * these accumulate before being divided down and reset (a couple thousand
 * frames' worth of microsecond sums at most), which comfortably fits 32
 * bits -- and keeps every division below at a 32-by-32 divide. The ELF
 * loader's symbol table exports no 64-bit integer divide helpers
 * (__udivdi3/__umoddi3, see the NES_FRAME_PERIOD_US comment above and the
 * matching one in nes_sound.c), so a uint64_t/uint32_t divide here would
 * build fine but fail to relocate at load time. */
static uint32_t s_perf_sum_iter_us;
static uint32_t s_perf_sum_blit_us;
/* nes_sound_last_write_us() reflects the *previous* iteration's pace_frame()
 * -> nes_sound_pump() call (this function runs before pace_frame() in
 * video_blit(), so this iteration's own write hasn't happened yet) -- a
 * one-iteration lag that washes out in a rolling average over
 * NES_PERF_REPORT_INTERVAL_MS. Folded in on every iteration, drawn or
 * skipped, since nes_sound_pump() runs unconditionally from pace_frame(). */
static uint32_t s_perf_sum_audio_us;
static uint32_t s_perf_sum_batches_drawn;
static uint32_t s_perf_sum_batches_total;

/* Folds this iteration's timing into the rolling counters and, once
 * NES_PERF_REPORT_INTERVAL_MS has elapsed, prints and resets them. Called
 * from video_blit() so it shares that function's `now_ms` reading rather
 * than taking a fresh one. */
static void perf_track_and_report(
    uint64_t now_ms, bool stall, uint32_t blit_us, bool drew, uint32_t batches_drawn, uint32_t batches_total
) {
    if (drew) {
        s_perf_frames_drawn++;
        s_perf_sum_blit_us += blit_us;
        s_perf_sum_batches_drawn += batches_drawn;
        s_perf_sum_batches_total += batches_total;
    } else {
        s_perf_frames_skipped++;
    }
    s_perf_sum_audio_us += nes_sound_last_write_us();
    if (!stall && s_last_iter_ms != 0) {
        s_perf_sum_iter_us += (uint32_t)(now_ms - s_last_iter_ms) * 1000u;
    }

    if (s_perf_report_ms == 0) {
        s_perf_report_ms = now_ms;
        return;
    }
    /* Safe to narrow: this only runs once NES_PERF_REPORT_INTERVAL_MS (a
     * couple seconds) has elapsed, never accumulated across a long idle
     * gap -- video_blit() updates s_perf_report_ms via this same call every
     * iteration, stall or not. */
    uint32_t window_ms = (uint32_t)(now_ms - s_perf_report_ms);
    if (window_ms < NES_PERF_REPORT_INTERVAL_MS) return;

    uint32_t total_frames = s_perf_frames_drawn + s_perf_frames_skipped;
    if (total_frames > 0 && window_ms > 0) {
        uint32_t avg_iter_us = s_perf_sum_iter_us / total_frames;
        uint32_t avg_blit_us = s_perf_frames_drawn > 0 ? s_perf_sum_blit_us / s_perf_frames_drawn : 0;
        uint32_t avg_audio_us = s_perf_sum_audio_us / total_frames;
        /* x100 for one fractional digit without floating point or a 64-bit
         * divide (total_frames * 100000 comfortably fits uint32_t at this
         * report interval and any plausible frame rate). */
        uint32_t fps_x100 = total_frames * 100000u / window_ms;
        printf(
            "nes: %lu.%02lu fps  iter=%luus (budget %luus)  blit=%luus  audio_write=%luus  skipped=%lu/%lu  "
            "batches=%lu/%lu\n",
            (unsigned long)(fps_x100 / 100u),
            (unsigned long)(fps_x100 % 100u),
            (unsigned long)avg_iter_us,
            (unsigned long)NES_FRAME_PERIOD_US,
            (unsigned long)avg_blit_us,
            (unsigned long)avg_audio_us,
            (unsigned long)s_perf_frames_skipped,
            (unsigned long)total_frames,
            (unsigned long)s_perf_sum_batches_drawn,
            (unsigned long)s_perf_sum_batches_total
        );
    }
    s_perf_report_ms = now_ms;
    s_perf_frames_drawn = 0;
    s_perf_frames_skipped = 0;
    s_perf_sum_iter_us = 0;
    s_perf_sum_blit_us = 0;
    s_perf_sum_audio_us = 0;
    s_perf_sum_batches_drawn = 0;
    s_perf_sum_batches_total = 0;
}

/* Updates s_skip_debt_us from this iteration's real duration and returns
 * true if this frame's blit should be skipped. Must be called once per
 * video_blit() with a fresh `now_ms`, before s_last_iter_ms is overwritten. */
static bool skip_track_debt(uint64_t now_ms, bool stall) {
    if (stall) {
        s_skip_debt_us = 0;
    } else if (s_last_iter_ms != 0) {
        uint32_t iter_us = (uint32_t)(now_ms - s_last_iter_ms) * 1000u;
        if (iter_us > NES_FRAME_PERIOD_US) {
            uint32_t over_us = iter_us - NES_FRAME_PERIOD_US;
            s_skip_debt_us = (s_skip_debt_us + over_us > NES_SKIP_MAX_DEBT_US) ? NES_SKIP_MAX_DEBT_US
                                                                               : s_skip_debt_us + over_us;
        } else {
            /* Running ahead of budget doesn't bank credit -- pace_frame()'s
             * own delay already handles that case -- but it does pay down
             * existing debt, so a few comfortably-fast frames after a rough
             * patch stop skipping again promptly. */
            uint32_t under_us = NES_FRAME_PERIOD_US - iter_us;
            s_skip_debt_us = under_us >= s_skip_debt_us ? 0 : s_skip_debt_us - under_us;
        }
    }

    if (s_skip_debt_us >= NES_FRAME_PERIOD_US) {
        s_skip_debt_us -= NES_FRAME_PERIOD_US;
        return true;
    }
    return false;
}

/* Minimum real time between the forced watchdog-safety yields below. This
 * project's FreeRTOS tick is 10ms (CONFIG_FREERTOS_HZ=100), not the 1ms the
 * name `runtime__delay(1)` suggests: process__wait_ms() converts ms to ticks
 * via pdMS_TO_TICKS(), and 1ms rounds down to 0 ticks at 100Hz, which it then
 * bumps back up to a 1-tick minimum -- so every call actually blocks a full
 * 10ms, not ~1ms. On-device measurement (nes_video.c's perf report) showed
 * this branch firing on essentially every iteration once emulation falls
 * behind budget (which, per that same measurement, is always), making a
 * "yield" that was meant to cost nothing instead cost ~10ms/frame -- close
 * to the entire remaining, otherwise-unexplained gap between iter and
 * blit+audio_write. The watchdog only needs IDLE0 (this task is CPU0-pinned)
 * fed once every CONFIG_ESP_TASK_WDT_TIMEOUT_S (5s in this project's
 * sdkconfig); throttling to one real yield per this interval keeps a huge
 * (10x+) safety margin while amortizing the 10ms cost from every frame down
 * to a small fraction of one. */
#define NES_YIELD_INTERVAL_MS 250u

static void pace_frame(void) {
    static uint64_t next_frame_ms;
    static uint32_t next_frame_remainder_us;
    static uint64_t last_yield_ms;

    uint64_t now_ms = runtime__now();
    if (next_frame_ms == 0) next_frame_ms = now_ms;
    next_frame_ms += NES_FRAME_PERIOD_US / 1000u;
    next_frame_remainder_us += NES_FRAME_PERIOD_US % 1000u;
    if (next_frame_remainder_us >= 1000u) {
        next_frame_remainder_us -= 1000u;
        next_frame_ms += 1;
    }
    if (next_frame_ms > now_ms) {
        runtime__delay((uint32_t)(next_frame_ms - now_ms));
        last_yield_ms = now_ms;
    } else {
        if (now_ms > next_frame_ms && now_ms - next_frame_ms > 100) {
            next_frame_ms = now_ms;
            next_frame_remainder_us = 0;
        }
        /* Emulation + blit ran over budget: there is no actual idle time to
         * spend. This task must still hit a real scheduling point periodically
         * so it doesn't starve IDLE0 and trip the task watchdog -- but not on
         * every single frame (see NES_YIELD_INTERVAL_MS above); most
         * over-budget frames fall through here with no delay at all. */
        if (now_ms - last_yield_ms >= NES_YIELD_INTERVAL_MS) {
            runtime__delay(1);
            last_yield_ms = now_ms;
        }
    }
    if (s_timer_callback != NULL) s_timer_callback();
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

/* nofrendo's own dirty-rect tracking is dead code: vid_flush() in vid_drv.c
 * hardcodes num_dirties = -1 ("full blit required") unconditionally --
 * calc_dirties() is commented out -- so every video_blit() call repaints the
 * entire visible area regardless of how much of it actually changed. On
 * real hardware this file's own blit (palette conversion + DMA) is the
 * single largest piece of frame time, bigger than CPU+PPU emulation, so
 * skipping the untouched part of it is worth doing here even without
 * upstream dirty-rect support.
 *
 * Storing a full previous-frame copy to diff against would cost ~56KB
 * (256x224 source bytes) -- the same order of magnitude as the cache-size
 * bump that was tried and reverted for its RAM cost with no benefit. A
 * cheap per-batch checksum (FNV-1a over exactly the source bytes that batch
 * would read) costs a few hundred bytes instead, at the cost of a small
 * chance of an accidental hash collision letting a genuinely-changed batch
 * go unpainted for one frame -- acceptable for a non-adversarial pixel
 * source, and self-correcting the moment that batch's content changes
 * again. Sized generously (up to 512px of draw_height); batches beyond this
 * many simply never get the shortcut, they just always redraw -- safe, not
 * a correctness issue. */
#define NES_MAX_BLIT_BATCHES ((512 + BRUCE_NES_BLIT_ROWS - 1) / BRUCE_NES_BLIT_ROWS)
static uint32_t s_row_batch_hash[NES_MAX_BLIT_BATCHES];
static bool s_row_batch_valid[NES_MAX_BLIT_BATCHES];

static uint32_t fnv1a_hash(const uint8_t *data, int len, uint32_t hash) {
    while (len-- > 0) {
        hash ^= *data++;
        hash *= 16777619u;
    }
    return hash;
}

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

    uint64_t now_ms = runtime__now();
    bool stall = s_last_iter_ms != 0 && (now_ms - s_last_iter_ms) >= NES_PERF_STALL_MS;
    bool skip_blit = skip_track_debt(now_ms, stall);

    int screen_width = display__width();
    int screen_height = display__height();
    if (screen_width <= 0 || screen_height <= 0) {
        s_last_iter_ms = now_ms;
        pace_frame();
        return;
    }

    if (skip_blit) {
        perf_track_and_report(now_ms, stall, 0, false, 0, 0);
        s_last_iter_ms = now_ms;
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

    uint32_t blit_us = 0;
    if (display__begin_frame() == BRUCE_OK) {
        uint64_t blit_start_ms = runtime__now();
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
            /* Geometry changed, so draw_width/source_x/the y_step mapping
             * below did too -- every stored batch checksum was computed
             * against the old geometry's byte ranges and no longer means
             * anything. Force every batch to redraw once so the new
             * geometry actually appears, then resume comparing normally. */
            memset(s_row_batch_valid, 0, sizeof(s_row_batch_valid));
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

        uint32_t batches_drawn = 0;
        uint32_t batches_total = 0;
        int batch_index = 0;
        for (int y = 0; y < draw_height; y += BRUCE_NES_BLIT_ROWS, ++batch_index) {
            int rows = draw_height - y;
            if (rows > BRUCE_NES_BLIT_ROWS) rows = BRUCE_NES_BLIT_ROWS;
            batches_total++;

            /* Skip this batch entirely -- no palette conversion, no DMA --
             * if the exact source bytes it would read are byte-for-byte
             * identical to the last batch actually drawn at this slot (see
             * the NES_MAX_BLIT_BATCHES comment above for why a checksum
             * instead of a stored copy). Batches beyond the checksum
             * table's size (screens taller than 512px) always fall through
             * and redraw -- there's no slot to compare against, not a
             * correctness issue. */
            bool have_slot = batch_index < NES_MAX_BLIT_BATCHES;
            if (have_slot) {
                uint32_t hash = 2166136261u; /* FNV-1a 32-bit offset basis */
                for (int row = 0; row < rows; ++row) {
                    int src_row = source_y + (((uint32_t)(y + row) * y_step) >> 16);
                    hash = fnv1a_hash(bitmap->line[src_row] + source_x, draw_width, hash);
                }
                if (s_row_batch_valid[batch_index] && s_row_batch_hash[batch_index] == hash) continue;
                s_row_batch_hash[batch_index] = hash;
                s_row_batch_valid[batch_index] = true;
            }

            for (int row = 0; row < rows; ++row) {
                int src_row = source_y + (((uint32_t)(y + row) * y_step) >> 16);
                const uint8_t *source = bitmap->line[src_row] + source_x;
                uint16_t *dst = &s_scaled_lines[row * draw_width];
                for (int x = 0; x < draw_width; ++x) { dst[x] = s_palette[source[x]]; }
            }
            display__draw_rgb_bitmap(origin_x, origin_y + y, s_scaled_lines, draw_width, rows);
            batches_drawn++;
        }
        display__present();
        blit_us = (uint32_t)(runtime__now() - blit_start_ms) * 1000u;
        perf_track_and_report(now_ms, stall, blit_us, true, batches_drawn, batches_total);
        s_last_iter_ms = now_ms;
        pace_frame();
        return;
    }
    perf_track_and_report(now_ms, stall, blit_us, true, 0, 0);
    s_last_iter_ms = now_ms;
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
