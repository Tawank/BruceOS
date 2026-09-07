// Bruce platform backend for doomgeneric's video hooks (DG_Init/DG_DrawFrame
// -- see doomgeneric/doomgeneric.h). This is the only Doom-side video code:
// everything upstream of it (the software renderer, palette handling, the
// indexed-to-RGB565 conversion) is unmodified vendored source in
// doomgeneric/i_video.c, driven entirely by the "-gfxmode rgb565" argument
// main.c passes to doomgeneric_Create() -- see main.c's comment for why.
//
// DOOMGENERIC_RESX/RESY (below) are pinned to Doom's own native 320x200
// resolution rather than the real display's, so i_video.c's own internal
// scale factor (fb_scaling = xres/SCREENWIDTH) always comes out to 1 and
// DG_ScreenBuffer holds a flat, unscaled 320x200 RGB565 image every frame --
// this file does the one scale-blit to the actual (runtime-queried, never
// hardcoded -- see core_sdk/display.h) screen size itself, the same
// division of labor nes_video.c uses for the NES's fixed 256x224 buffer.

#define DOOMGENERIC_RESX 320
#define DOOMGENERIC_RESY 200

#include <stdint.h>
#include <stdio.h>

#include "core_sdk/display.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"

#include "doomgeneric.h"

/* Rows batched into one display__draw_rgb_bitmap() call. Blitting a single
 * scanline at a time turns every frame into one DMA transaction per screen
 * row, each paying its own address-window/ISR round-trip and becoming
 * visible on the physical panel the instant it lands -- the same
 * top-to-bottom "drawing itself" effect nes_video.c's BRUCE_NES_BLIT_ROWS
 * comment describes and fixes the same way: batch several rows per
 * transfer. The buffer is sized at runtime to the active display width. */
#define DOOM_BLIT_ROWS 32

static uint16_t *s_scaled_rows;
static size_t s_scaled_rows_capacity;

void DG_Init(void) { printf("doom: video ready (%dx%d source)\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY); }

void DG_SetWindowTitle(const char *title) { (void)title; }

uint32_t DG_GetTicksMs(void) { return (uint32_t)runtime__now(); }

void DG_SleepMs(uint32_t ms) { runtime__delay(ms); }

/* Same rationale as nes_video.c's NES_YIELD_INTERVAL_MS: this task must hit
 * a real FreeRTOS scheduling point periodically or it starves IDLE0 and
 * trips the task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S, 5s in this
 * project), but doing that on every single frame would cost a full 10ms
 * tick each time (CONFIG_FREERTOS_HZ=100 rounds any runtime__delay() up to
 * one tick). DG_SleepMs() above already yields whenever Doom's own pacing
 * (I_Sleep(), called from TryRunTics() in d_loop.c) decides there's real
 * idle time to spend, but that's not guaranteed to happen often if a level
 * is heavy enough to keep the renderer behind every tic -- so this throttled
 * yield here, run once per drawn frame regardless, is the actual watchdog
 * safety net. */
#define DOOM_YIELD_INTERVAL_MS 250u
static uint64_t s_last_yield_ms;

void DG_DrawFrame(void) {
    uint64_t now_ms = runtime__now();
    if (now_ms - s_last_yield_ms >= DOOM_YIELD_INTERVAL_MS) {
        runtime__delay(1);
        s_last_yield_ms = now_ms;
    }

    int screen_width = display__width();
    int screen_height = display__height();
    if (screen_width <= 0 || screen_height <= 0 || DG_ScreenBuffer == NULL) return;

    if (display__begin_frame() != BRUCE_OK) return;

    const uint16_t *source = (const uint16_t *)DG_ScreenBuffer;

    if (screen_width == DOOMGENERIC_RESX && screen_height == DOOMGENERIC_RESY) {
        /* Exact match (or a device that happens to run at Doom's native
         * resolution) -- skip the resample loop below entirely. */
        display__draw_rgb_bitmap(0, 0, source, screen_width, screen_height);
        display__present();
        return;
    }

    /* Nearest-neighbor scale to fill the screen, one destination row at a
     * time via a 16.16 fixed-point step on both axes -- there is no
     * hardware integer divider on this target (see the ELF loader's 64-bit
     * divide constraint noted in elf_loader_sdk_symbols.c and nes_video.c),
     * so the ratios are computed once here with ordinary 32-by-32 division
     * (screen_width/height and DOOMGENERIC_RESX/RESY are always small plain
     * ints, never a 64-bit divide) and then walked per-pixel with only
     * shifts and adds. */
    size_t rows_needed = (size_t)DOOM_BLIT_ROWS * (size_t)screen_width;
    if (rows_needed > s_scaled_rows_capacity) {
        uint16_t *scaled_rows = memory__realloc(s_scaled_rows, rows_needed * sizeof(*scaled_rows));
        if (scaled_rows == NULL) {
            display__present();
            return;
        }
        s_scaled_rows = scaled_rows;
        s_scaled_rows_capacity = rows_needed;
    }

    uint32_t x_step = ((uint32_t)DOOMGENERIC_RESX << 16) / (uint32_t)screen_width;
    uint32_t y_step = ((uint32_t)DOOMGENERIC_RESY << 16) / (uint32_t)screen_height;

    for (int y = 0; y < screen_height; y += DOOM_BLIT_ROWS) {
        int rows = screen_height - y;
        if (rows > DOOM_BLIT_ROWS) rows = DOOM_BLIT_ROWS;

        for (int row = 0; row < rows; ++row) {
            int src_row = (int)(((uint32_t)(y + row) * y_step) >> 16);
            if (src_row >= DOOMGENERIC_RESY) src_row = DOOMGENERIC_RESY - 1;
            const uint16_t *src = source + (size_t)src_row * DOOMGENERIC_RESX;
            uint16_t *dst = &s_scaled_rows[(size_t)row * screen_width];

            uint32_t x_accum = 0;
            for (int x = 0; x < screen_width; ++x) {
                int src_col = (int)(x_accum >> 16);
                if (src_col >= DOOMGENERIC_RESX) src_col = DOOMGENERIC_RESX - 1;
                dst[x] = src[src_col];
                x_accum += x_step;
            }
        }
        display__draw_rgb_bitmap(0, y, s_scaled_rows, screen_width, rows);
    }
    display__present();
}
