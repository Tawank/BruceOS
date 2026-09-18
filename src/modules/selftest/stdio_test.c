#include "stdio_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

/* Exercises stdio__session_set_raw() (core/stdio/stdio.c): a fresh session
 * still gets ONLCR translation ('\n' -> "\r\n") on its output, same as
 * before this existed, but marking it raw turns that off -- the fix for
 * "curl url | image" corrupting piped binary data (a PNG's own signature
 * contains a 0x0A byte) by inserting a spurious '\r' ahead of every 0x0A in
 * shell_executor.c's pipe/capture/redirect sessions. Talks to the session
 * directly via stdio__write_to()/stdio__session_read_output() rather than
 * routing this process's own stdio through it, so it needs no child process
 * and isn't subject to the QEMU external-heap/PSRAM comparison caveat that
 * shell_test.c's own pipe/redirect content checks carry (see
 * selftest__run_shell_pipe_redirect_case()'s doc comment) -- this session's
 * ring buffer is a plain calloc()'d field, not memory__external_malloc()-backed. */
bool selftest__run_stdio_raw_session_case(void) {
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    bool ok = stdio__session_create(&session) == BRUCE_OK;

    static const char input[] = "a\nb\n";
    char translated[16] = {0};
    size_t translated_size = 0;
    ok = ok && stdio__write_to(session, input, sizeof(input) - 1) == BRUCE_OK &&
         stdio__session_read_output(session, translated, sizeof(translated), &translated_size) == BRUCE_OK &&
         translated_size == 6 && memcmp(translated, "a\r\nb\r\n", 6) == 0;

    char raw_text[16] = {0};
    size_t raw_size = 0;
    ok = ok && stdio__session_set_raw(session, true) == BRUCE_OK &&
         stdio__write_to(session, input, sizeof(input) - 1) == BRUCE_OK &&
         stdio__session_read_output(session, raw_text, sizeof(raw_text), &raw_size) == BRUCE_OK &&
         raw_size == sizeof(input) - 1 && memcmp(raw_text, input, sizeof(input) - 1) == 0;

    /* An id nothing created (session ids start at 1 and increment per
     * process lifetime) must be rejected rather than silently accepted. */
    bruce_stdio_session_t bogus = session + 1000000u;
    ok = ok && stdio__session_set_raw(bogus, true) == BRUCE_ERR_NOT_FOUND;

    if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
    printf("[selftest] stdio/raw_session: %s\n", ok ? "OK" : "failed");
    return ok;
}
