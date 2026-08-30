/* nofrendo OSD glue: process lifecycle, memory, config stub and filename
 * plumbing -- everything nofrendo's osd.h interface needs that isn't video
 * (nes_video.c), input (nes_input.c) or audio (nes_sound.c). Mirrors how the
 * old Arduino port kept its own osd.c to just this kind of glue, with
 * display/controller/sound reads living in their own files. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"

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

/* mem_commit_readonly()/mem_free_readonly() back the ROM and VROM bank
 * images rom_loadrom() (nes_rom.c) loads once via mem_alloc()+fread() and
 * never writes again -- unlike the sram/vram buffers, which stay on plain
 * mem_alloc()/NOFRENDO_FREE() because the emulator does keep writing
 * through those. memory__external_malloc()'s PSRAM-preferred, swap-backed
 * allocator fits that read-only-after-load shape better than the
 * general-purpose tracked heap mem_alloc() routes everything else through,
 * but the pointer it returns is only safely written to through
 * memory__external_memcpy() -- so committing a freshly loaded buffer means
 * allocating a second, external buffer, copying the bytes across, and
 * swapping the pointer nofrendo keeps from then on.
 *
 * At most two such regions are ever live at once (one ROM, one VROM), so a
 * two-slot table is all the bookkeeping mem_free_readonly() needs to tell
 * "this pointer migrated to an external allocation" apart from "commit fell
 * back to the original mem_alloc()'d buffer" (PSRAM/swap unavailable, or
 * both slots already taken by a still-loaded ROM). */
#define NES_OSD__READONLY_SLOTS 2
static const void *s_readonly_pointers[NES_OSD__READONLY_SLOTS];

void *mem_commit_readonly(void *buffer, int size) {
    if (buffer == NULL || size <= 0) return buffer;

    int slot = -1;
    for (int i = 0; i < NES_OSD__READONLY_SLOTS; ++i) {
        if (s_readonly_pointers[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return buffer;

    const void *data = memory__external_malloc((size_t)size);
    if (data == NULL) return buffer;
    if (memory__external_memcpy(data, 0, buffer, (size_t)size) != BRUCE_OK) {
        (void)memory__external_free(data);
        return buffer;
    }

    memory__free(buffer);
    s_readonly_pointers[slot] = data;
    return (void *)data;
}

void mem_free_readonly(void *buffer, int size) {
    (void)size;
    if (buffer == NULL) return;
    for (int i = 0; i < NES_OSD__READONLY_SLOTS; ++i) {
        if (s_readonly_pointers[i] == buffer) {
            (void)memory__external_free(s_readonly_pointers[i]);
            s_readonly_pointers[i] = NULL;
            return;
        }
    }
    /* Never migrated (alloc/memcpy failed, or both slots were taken) --
     * still the original mem_alloc()'d buffer. */
    memory__free(buffer);
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

static bruce_timer_id_t s_nofrendo_timer;

int osd_init(void) { return 0; }

void osd_shutdown(void) {
    /* nofrendo never calls osd_stopsound() itself (it's dead weight left
     * over from another port's OSD interface), so this is the only in-band
     * chance to close the stream before the process exits. If it's missed
     * (a crash, a forced kill), Core's automatic per-process resource
     * cleanup (see audio__stream_open() in core_sdk/audio.h) closes it
     * anyway. See nes_sound.c for the definition. */
    osd_stopsound();
    if (s_nofrendo_timer != BRUCE_TIMER_ID_INVALID) {
        (void)runtime__timer_stop(s_nofrendo_timer);
        s_nofrendo_timer = BRUCE_TIMER_ID_INVALID;
    }
    nes_video_destroy_bitmap();
}

int osd_main(int argc, char *argv[]) {
    if (argc < 1 || argv == NULL || argv[0] == NULL) return -1;
    return main_loop(argv[0], system_autodetect);
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize) {
    (void)func;
    (void)funcsize;
    if (frequency <= 0 || counter == NULL || countersize != (int)sizeof(uint32_t)) return -1;
    if (s_nofrendo_timer != BRUCE_TIMER_ID_INVALID) (void)runtime__timer_stop(s_nofrendo_timer);
    s_nofrendo_timer = BRUCE_TIMER_ID_INVALID;
    return runtime__timer_start(1000000u / (uint32_t)frequency, (volatile uint32_t *)counter, &s_nofrendo_timer) ==
                   BRUCE_OK
               ? 0
               : -1;
}

void osd_waittimer(void) {
    if (s_nofrendo_timer != BRUCE_TIMER_ID_INVALID) {
        (void)runtime__timer_wait(s_nofrendo_timer, UINT32_MAX);
    }
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
