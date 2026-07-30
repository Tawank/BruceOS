#include <stdio.h>
#include <string.h>

#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "nofrendo.h"

int app_main(int argc, char **argv) {
    char selected_path[192];
    const char *rom_path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], "--gui") != 0) {
            rom_path = argv[i];
            break;
        }
    }

    if (rom_path == NULL) {
        if (dialog__pick_file("/", ".nes", selected_path, sizeof(selected_path)) != BRUCE_OK) return 0;
        rom_path = selected_path;
    }

    printf("Nofrendo: loading %s\n", rom_path);
    char *nofrendo_argv[] = {(char *)rom_path};
    return nofrendo_main(1, nofrendo_argv);
}
