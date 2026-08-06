/* nofrendo OSD glue: process lifecycle, memory, config stub and filename
 * plumbing -- everything nofrendo's osd.h interface needs that isn't video
 * (nes_video.c), input (nes_input.c) or audio (nes_sound.c). Mirrors how the
 * old Arduino port kept its own osd.c to just this kind of glue, with
 * display/controller/sound reads living in their own files. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core_sdk/memory.h"

#include "nes/nes.h"
#include "nes_sound.h"
#include "nes_video.h"
#include "nofconfig.h"
#include "nofrendo.h"
#include "osd.h"

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

int osd_init(void) { return 0; }

void osd_shutdown(void) {
    /* nofrendo never calls osd_stopsound() itself (it's dead weight left
     * over from another port's OSD interface), so this is the only in-band
     * chance to close the stream before the process exits. If it's missed
     * (a crash, a forced kill), Core's automatic per-process resource
     * cleanup (see audio__stream_open() in core_sdk/audio.h) closes it
     * anyway. */
    osd_stopsound();
    nes_video_destroy_bitmap();
}

int osd_main(int argc, char *argv[]) {
    if (argc < 1 || argv == NULL || argv[0] == NULL) return -1;
    return main_loop(argv[0], system_autodetect);
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize) {
    (void)frequency;
    (void)funcsize;
    (void)counter;
    (void)countersize;
    void (*callback)(void) = (void (*)(void))func;
    nes_video_install_timer(callback);
    if (callback != NULL) callback();

    /* nofrendo's nes_emulate() only renders a frame once nofrendo_ticks has
     * advanced past the value it last observed, and normally expects
     * osd_installtimer() to arm a real, independent periodic interrupt that
     * bumps nofrendo_ticks regardless of whether a frame is being rendered.
     * The ELF app SDK exposes no such primitive, so here nofrendo_ticks can
     * only ever advance from inside nes_video.c's pace_frame(), which only
     * runs *after* a frame render completes. With nes.autoframeskip left at
     * its nes_create() default of true, nes_emulate()'s "render one frame"
     * branch requires frames_to_render > 0, which requires nofrendo_ticks to
     * have already moved -- a cycle that never gets going after the
     * one-time bump above, so the emulator hangs in nes_emulate()'s idle
     * spin forever, starving whichever core it lands on and eventually
     * tripping the task watchdog. Forcing autoframeskip off makes
     * nes_emulate() render unconditionally every loop iteration instead of
     * waiting on the tick counter; pace_frame (via runtime__delay()) is then
     * the only thing pacing frame rate, which is exactly what it already
     * does. */
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
