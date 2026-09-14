#include "man_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "modules/utils/man/man_app.h"

/* "man kill": exercises man_app.c's new "describe a shell built-in" path
 * (man_app__describe_builtin(), reached from man_app_main() when the name
 * isn't a registered app_runner command but is a shell builtin -- see
 * shell_jobs__kill()) -- "kill" has no process of its own for man's usual
 * "<command> --help" paths to run against. Captures man_app_main()'s own
 * stdio__printf() output the same way shell_executor__builtin_redirected()
 * captures a builtin's output for "builtin > file": there's no child
 * process here to relay from, just this task's own writes, so
 * stdio__session_capture_self() reroutes them directly. */
bool selftest__run_man_builtin_case(void) {
    bruce_stdio_session_t capture = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&capture) != BRUCE_OK || stdio__session_capture_self(capture) != BRUCE_OK) {
        if (capture != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(capture);
        printf("[selftest] man/builtin: capture setup failed\n");
        return false;
    }
    char *argv[] = {"man", "kill", NULL};
    int status = man_app_main(2, argv);
    (void)stdio__session_release_self();

    char text[256];
    size_t total = 0;
    for (;;) {
        char chunk[128];
        size_t size = 0;
        if (stdio__session_read_output(capture, chunk, sizeof(chunk), &size) != BRUCE_OK || size == 0) break;
        size_t space = sizeof(text) - 1 - total;
        size_t copy = size < space ? size : space;
        memcpy(text + total, chunk, copy);
        total += copy;
        if (total >= sizeof(text) - 1) break;
    }
    text[total] = '\0';
    (void)stdio__session_close(capture);

    bool ok = status == BRUCE_OK && strstr(text, "kill - Send a signal to a job or process") != NULL &&
              strstr(text, "shell built-in") != NULL;
    if (!ok) printf("[selftest] man/builtin: status=%d text=%s\n", status, text);
    printf("[selftest] man/builtin: %s\n", ok ? "OK" : "failed");
    return ok;
}
