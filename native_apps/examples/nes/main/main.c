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
        if (dialog__pick_file("/", ".nes", selected_path, sizeof(selected_path), "NES") != BRUCE_OK) return 0;
        rom_path = selected_path;
    }

    printf("Nofrendo: loading %s\n", rom_path);
    char *nofrendo_argv[] = {(char *)rom_path};

    int result = nofrendo_main(1, nofrendo_argv);

    /* main_loop() (nofrendo.c) only ever returns nonzero from a failed
     * internal_insert() -- bad/unsupported ROM, a mapper this port doesn't
     * implement, or an allocation (ROM buffer, framebuffer, ...) that
     * failed -- a clean exit (console.quit) always returns 0. nofrendo's own
     * diagnostics for exactly this (nofrendo_log_printf) are compiled out to
     * a no-op unless NOFRENDO_DEBUG is defined (see log.c), so without this,
     * every one of those failures was completely silent: the process just
     * exited back to the launcher with no indication anything went wrong. */
    if (result != 0) {
        printf("Nofrendo: failed to start '%s' (result=%d)\n", rom_path, result);
        dialog__message(
            BRUCE_DIALOG_ERROR, "NES failed to start",
            "Could not start this ROM. It may be invalid or use an unsupported mapper, or there may not be "
            "enough free RAM."
        );
    }

    return result;
}
