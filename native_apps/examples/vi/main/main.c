#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

#include "libbb.h"

int vi_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;

/* Same "no argument -> file picker rooted at /" convention as
 * doom/main.c and nes/main.c, just without an extension filter (vi opens
 * any file, or none at all -- ":e" or plain "vi" both work in real vi
 * too). getopt32()'s own argv-walking contract (see libbb.h) needs a
 * NULL-terminated array; this one always is, regardless of what BruceOS's
 * own argv (forwarded past it unused otherwise) looks like. */
int app_main(int argc, char **argv) {
    char selected_path[192];
    const char *file_path = argc > 1 ? argv[1] : NULL;
    if (file_path == NULL) {
        if (dialog__pick_file("/", NULL, selected_path, sizeof(selected_path), "Open") == BRUCE_OK) {
            file_path = selected_path;
        }
        /* No selection (picker cancelled) -- fall through with a NULL
         * path, same as running plain "vi" with no file argument at a
         * real shell: it opens into an empty, unnamed buffer. */
    }

    char *vi_argv[3];
    int vi_argc = 0;
    vi_argv[vi_argc++] = "vi";
    if (file_path != NULL) vi_argv[vi_argc++] = (char *)file_path;
    vi_argv[vi_argc] = NULL;

    return vi_main(vi_argc, vi_argv);
}
