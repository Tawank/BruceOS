#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"

#include "bitmap.h"
#include "event.h"
#include "gui.h"
#include "nes/nes.h"
#include "nes/nes_ppu.h"
#include "nes/nesinput.h"
#include "nofconfig.h"
#include "nofrendo.h"
#include "osd.h"

static uint8_t framebuffer_dummy;
static bitmap_t *screen_bitmap;
static uint16_t palette[256];
static uint64_t next_frame_ms;
static uint32_t next_frame_remainder_us;
static void (*timer_callback)(void);

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
static uint16_t scaled_lines[BRUCE_NES_BLIT_ROWS * BRUCE_NES_BLIT_MAX_WIDTH];

/* Last geometry the letterbox borders were cleared for -- see video_blit()'s
 * border clear below. Impossible values force a clear on the first frame. */
static int s_last_origin_x = -1;
static int s_last_origin_y = -1;
static int s_last_draw_width = -1;
static int s_last_draw_height = -1;
static int s_last_screen_width = -1;
static int s_last_screen_height = -1;

void *mem_alloc(int size, bool prefer_fast_memory) {
    (void)prefer_fast_memory;
    return size > 0 ? memory__malloc((size_t)size) : NULL;
}

static bool config_open(void) { return false; }
static void config_close(void) {}
static int config_read_int(const char *group, const char *key, int value) {
    (void)group;
    (void)key;
    return value;
}
static const char *config_read_string(const char *group, const char *key, const char *value) {
    (void)group;
    (void)key;
    return value;
}
static void config_write_int(const char *group, const char *key, int value) {
    (void)group;
    (void)key;
    (void)value;
}
static void config_write_string(const char *group, const char *key, const char *value) {
    (void)group;
    (void)key;
    (void)value;
}

config_t config = {
    config_open,
    config_close,
    config_read_int,
    config_read_string,
    config_write_int,
    config_write_string,
    NULL,
};

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
    for (int i = 0; i < 256; ++i) { palette[i] = display__color565(colors[i].r, colors[i].g, colors[i].b); }
}
static void video_clear(uint8 color) {
    (void)color;
    if (display__begin_frame() == BRUCE_OK) {
        display__fill_screen(BRUCE_COLOR_BLACK);
        display__present();
    }
}
static bitmap_t *video_lock(void) {
    if (screen_bitmap == NULL) {
        screen_bitmap =
            bmp_createhw(&framebuffer_dummy, NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, NES_SCREEN_WIDTH);
    }
    return screen_bitmap;
}
static void video_free(int num_dirties, rect_t *dirty_rects) {
    (void)num_dirties;
    (void)dirty_rects;
}

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

static void pace_frame(void) {
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
    } else {
        if (now_ms > next_frame_ms && now_ms - next_frame_ms > 100) {
            next_frame_ms = now_ms;
            next_frame_remainder_us = 0;
        }
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
    if (timer_callback != NULL) timer_callback();
}

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
                uint16_t *dst = &scaled_lines[row * draw_width];
                for (int x = 0; x < draw_width; ++x) { dst[x] = palette[source[x]]; }
            }
            display__draw_rgb_bitmap(origin_x, origin_y + y, scaled_lines, draw_width, rows);
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

static int event_for_code(int32_t code) {
    switch (code) {
        case BRUCE_INPUT_CODE_UP: return event_joypad1_up;
        case BRUCE_INPUT_CODE_DOWN: return event_joypad1_down;
        case BRUCE_INPUT_CODE_LEFT: return event_joypad1_left;
        case BRUCE_INPUT_CODE_RIGHT: return event_joypad1_right;
        case BRUCE_INPUT_CODE_BUTTON_A:
        case 'z':
        case 'j': return event_joypad1_a;
        case BRUCE_INPUT_CODE_BUTTON_B:
        case 'x':
        case 'k': return event_joypad1_b;
        case BRUCE_INPUT_CODE_BUTTON_START:
        case '\n': return event_joypad1_start;
        case BRUCE_INPUT_CODE_BUTTON_SELECT:
        case ' ': return event_joypad1_select;
        case 'w': return event_joypad1_up;
        case 's': return event_joypad1_down;
        case 'a': return event_joypad1_left;
        case 'd': return event_joypad1_right;
        default: return 0;
    }
}

void osd_getinput(void) {
    bruce_input_event_t input;
    while (input__poll(&input) == BRUCE_OK) {
        if (input.action == BRUCE_INPUT_CHANGE) continue;
        int state = input.action == BRUCE_INPUT_PRESS ? INP_STATE_MAKE : INP_STATE_BREAK;
        if (input.code == BRUCE_INPUT_CODE_BACK && state == INP_STATE_MAKE) {
            event_t quit = event_get(event_quit);
            if (quit != NULL) quit(state);
            continue;
        }
        int event_code = event_for_code(input.code);
        event_t handler = event_code != 0 ? event_get(event_code) : NULL;
        if (handler != NULL) handler(state);
    }
}

void osd_getmouse(int *x, int *y, int *button) {
    if (x != NULL) *x = 0;
    if (y != NULL) *y = 0;
    if (button != NULL) *button = 0;
}

int osd_init(void) { return 0; }

void osd_shutdown(void) { bmp_destroy(&screen_bitmap); }

int osd_main(int argc, char *argv[]) {
    if (argc < 1 || argv == NULL || argv[0] == NULL) return -1;
    return main_loop(argv[0], system_autodetect);
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize) {
    (void)frequency;
    (void)funcsize;
    (void)counter;
    (void)countersize;
    timer_callback = (void (*)(void))func;
    if (timer_callback != NULL) timer_callback();

    /* nofrendo's nes_emulate() only renders a frame once nofrendo_ticks has
     * advanced past the value it last observed, and normally expects
     * osd_installtimer() to arm a real, independent periodic interrupt that
     * bumps nofrendo_ticks regardless of whether a frame is being rendered.
     * The ELF app SDK exposes no such primitive, so here nofrendo_ticks can
     * only ever advance from inside pace_frame(), which only runs *after* a
     * frame render completes. With nes.autoframeskip left at its nes_create()
     * default of true, nes_emulate()'s "render one frame" branch requires
     * frames_to_render > 0, which requires nofrendo_ticks to have already
     * moved -- a cycle that never gets going after the one-time bump above,
     * so the emulator hangs in nes_emulate()'s idle spin forever, starving
     * whichever core it lands on and eventually tripping the task watchdog.
     * Forcing autoframeskip off makes nes_emulate() render unconditionally
     * every loop iteration instead of waiting on the tick counter; pace_frame
     * (via runtime__delay(), see below) is then the only thing pacing frame
     * rate, which is exactly what it already does. */
    nes_t *machine = nes_getcontextptr();
    if (machine != NULL) machine->autoframeskip = false;

    return 0;
}

void osd_fullname(char *fullname, const char *shortname) {
    if (fullname == NULL) return;
    strncpy(fullname, shortname != NULL ? shortname : "", PATH_MAX);
    fullname[PATH_MAX] = '\0';
}

char *osd_newextension(char *path, char *extension) {
    if (path == NULL || extension == NULL) return path;
    char *slash = strrchr(path, '/');
    char *dot = strrchr(path, '.');
    if (dot == NULL || (slash != NULL && dot < slash)) dot = path + strlen(path);
    size_t available = PATH_MAX - (size_t)(dot - path);
    strncpy(dot, extension, available);
    path[PATH_MAX] = '\0';
    return path;
}

int osd_makesnapname(char *filename, int len) {
    (void)filename;
    (void)len;
    return -1;
}

int osd_init_sound(void) { return 0; }
void osd_stopsound(void) {}
void osd_setsound(void (*playfunc)(void *buffer, int length)) { (void)playfunc; }
void osd_getsoundinfo(sndinfo_t *info) {
    info->sample_rate = 22050;
    info->bps = 16;
}
