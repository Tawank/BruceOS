#include <stdio.h>

#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "nofrendo.h"

int app_main(int argc, char **argv) {
    char selected_path[192];
    const char *rom_path = argc > 1 ? argv[1] : NULL;

    if (rom_path == NULL) {
        if (dialog__pick_file("/", ".nes", selected_path, sizeof(selected_path)) != BRUCE_OK) return 0;
        rom_path = selected_path;
    }

    printf("Nofrendo: loading %s\n", rom_path);
    char *nofrendo_argv[] = {(char *)rom_path};

    /* nofrendo keeps its own emulated-console memory (PPU/CPU state, save
     * states, the ROM image itself) resident for as long as it runs -- on
     * top of whatever the Core display HAL is already holding for its own
     * off-screen framebuffer. Freeing that framebuffer for the emulator's
     * duration (game mode) hands nofrendo back the RAM it would otherwise
     * be competing for; nes_osd.c already drives the display purely through
     * begin_frame()/draw_rgb_bitmap()/present(), which work identically in
     * direct mode, so there is no rendering-path change here. Best-effort:
     * if game mode can't be entered (e.g. another process already owns it),
     * nofrendo still runs, just without the extra headroom. */
    bruce_result_t game_mode_result = display__game_mode(true);
    for (int attempt = 0; game_mode_result == BRUCE_ERR_BUSY && attempt < 10; ++attempt) {
        runtime__delay(10);
        game_mode_result = display__game_mode(true);
    }
    bool game_mode = game_mode_result == BRUCE_OK;
    if (!game_mode) printf("Nofrendo: display game mode unavailable (%d)\n", (int)game_mode_result);
    int result = nofrendo_main(1, nofrendo_argv);
    if (game_mode) display__game_mode(false);
    return result;
}
