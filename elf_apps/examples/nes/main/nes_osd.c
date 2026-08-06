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
static uint16_t scaled_line[320];
static uint64_t next_frame_ms;
static void (*timer_callback)(void);

#define BRUCE_NES_VISIBLE_HEIGHT 224

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

static void pace_frame(void) {
    uint64_t now = runtime__now();
    if (next_frame_ms == 0) next_frame_ms = now;
    next_frame_ms += 1000u / NES_REFRESH_RATE;
    if (next_frame_ms > now) {
        runtime__delay((uint32_t)(next_frame_ms - now));
    } else {
        if (now - next_frame_ms > 100) next_frame_ms = now;
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
    int draw_width = screen_width;
    int draw_height = draw_width * source_height / NES_SCREEN_WIDTH;
    if (draw_height > screen_height) {
        draw_height = screen_height;
        draw_width = draw_height * NES_SCREEN_WIDTH / source_height;
    }
    if (draw_width > (int)(sizeof(scaled_line) / sizeof(scaled_line[0]))) {
        draw_width = sizeof(scaled_line) / sizeof(scaled_line[0]);
    }
    int origin_x = (screen_width - draw_width) / 2;
    int origin_y = (screen_height - draw_height) / 2;

    if (display__begin_frame() == BRUCE_OK) {
        display__fill_screen(BRUCE_COLOR_BLACK);
        for (int y = 0; y < draw_height; ++y) {
            const uint8_t *source = bitmap->line[source_y + y * source_height / draw_height];
            for (int x = 0; x < draw_width; ++x) {
                scaled_line[x] = palette[source[x * NES_SCREEN_WIDTH / draw_width]];
            }
            display__draw_rgb_bitmap(origin_x, origin_y + y, scaled_line, draw_width, 1);
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
        case 'j': return event_joypad1_a;
        case BRUCE_INPUT_CODE_BUTTON_B:
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
