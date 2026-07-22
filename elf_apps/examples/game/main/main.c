#include <stdio.h>

int app_main(int argc, char **argv)
{
    printf("game started with %d args\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    return 0;
}
