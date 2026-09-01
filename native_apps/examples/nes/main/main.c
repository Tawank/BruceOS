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

    return result;
}
