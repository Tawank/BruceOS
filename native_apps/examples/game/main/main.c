#include "core_sdk/dialog.h"
#include <stdio.h>

int app_main(int argc, char **argv) {
    printf("game started with %d args\n", argc);
    for (int i = 0; i < argc; i++) { printf("  argv[%d] = %s\n", i, argv[i]); }

    char selected_path[192];
    const char *rom_path = NULL;

    if (rom_path == NULL) {
        if (dialog__pick_file("/", ".nes", selected_path, sizeof(selected_path), "Game") != BRUCE_OK) return 0;
        rom_path = selected_path;
    }

    printf("Nofrendo: loading %s\n", rom_path);
    return 0;
}
