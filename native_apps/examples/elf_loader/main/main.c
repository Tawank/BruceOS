#include <stdio.h>

#include "bruce_sdk.h"

int app_main(int argc, char **argv)
{
    printf("elf_loader started with %d args\n", argc);
    if (argc < 2) {
        printf("usage: elf_loader <elf_path>\n");
        return 0;
    }

    const char *target = argv[1];
    printf("elf_loader: loading %s\n", target);

    int result = app_runner__run_path(target, NULL, BRUCE_LAUNCH_FOREGROUND);
    printf("elf_loader: app_runner__run_path returned %d\n", result);
    return 0;
}
