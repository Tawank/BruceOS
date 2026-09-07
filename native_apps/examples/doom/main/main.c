#include <stdio.h>

#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

#include "doomgeneric.h"

/* Doom's own memory zone (Z_Init(), z_zone.c) is sized by I_ZoneBase() in
 * i_system.c: it takes "-mb <N>" as an exact N-MiB request (falling back to
 * a 6 MiB default with no argument at all) and just fails to start if that
 * much can't be malloc'd. This project's malloc() always draws from PSRAM
 * (see memory__malloc(), src/core/memory/memory.c), so this is a PSRAM
 * budget, not internal RAM -- 4 MiB comfortably fits the smallest PSRAM
 * size this project targets while leaving Doom enough headroom for a full
 * IWAD's textures/flats without swapping its own cache.
 *
 * "-gfxmode rgb565" selects doomgeneric/i_video.c's existing 16bpp output
 * path (its default is 32bpp RGBA) -- see doom_video.c's own comment for
 * why that's the one that needs zero patches to vendored source.
 * "-nogui" skips i_system.c's fallback error popup on a fatal I_Error(),
 * which would otherwise try to shell out to `zenity`; there's no shell
 * here (see elf_loader_sdk_symbols.c's bruce_elf__system() stub), so this
 * just avoids the wasted attempt. Doom still prints the error to stdout. */
int app_main(int argc, char **argv) {
    char selected_path[192];
    const char *wad_path = argc > 1 ? argv[1] : NULL;
    if (wad_path == NULL) {
        if (dialog__pick_file("/", ".wad", selected_path, sizeof(selected_path), "IWAD") != BRUCE_OK) return 0;
        wad_path = selected_path;
    }

    printf("doom: loading %s\n", wad_path);
    char *doom_argv[] = {"doom", "-iwad", (char *)wad_path, "-gfxmode", "rgb565", "-mb", "4", "-nogui"};
    doomgeneric_Create(sizeof(doom_argv) / sizeof(doom_argv[0]), doom_argv);

    /* doomgeneric_Create() runs Doom's own startup (D_DoomMain()) and
     * returns once initialization is done, having already pumped one tic
     * itself (see d_main.c's D_DoomLoop(), which calls doomgeneric_Tick()
     * exactly once at the end of setup) -- everything after this point is
     * this app's own responsibility to keep pumping, exactly like every
     * other doomgeneric platform port. Doom has no in-engine "return to
     * launcher" path (I_Quit() just runs atexit handlers -- see
     * i_system.c), so this loop runs for the lifetime of the process. */
    for (;;) { doomgeneric_Tick(); }
}
