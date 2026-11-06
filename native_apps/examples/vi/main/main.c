#include "libbb.h"

int vi_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;

/* No argument opens an empty, unnamed buffer, same as plain "vi" at a real
 * shell. getopt32()'s own argv-walking contract (see libbb.h) needs a
 * NULL-terminated array; this one always is, regardless of what BruceOS's
 * own argv (forwarded past it unused otherwise) looks like. */
int app_main(int argc, char **argv) {
    const char *file_path = argc > 1 ? argv[1] : NULL;

    char *vi_argv[3];
    int vi_argc = 0;
    vi_argv[vi_argc++] = "vi";
    if (file_path != NULL) vi_argv[vi_argc++] = (char *)file_path;
    vi_argv[vi_argc] = NULL;

    return vi_main(vi_argc, vi_argv);
}
