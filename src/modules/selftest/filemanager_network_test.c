/* Pure-logic unit coverage for filemanager_app.c's "/Network" folder
 * provider-registry parsing and display-name sanitizing -- no storage I/O,
 * no provider process spawned. See filemanager_network_internal.h for why
 * these are reachable from here (they're the parsing helpers factored out
 * of filemanager__network_load_providers()/_write_location() for exactly
 * this purpose). */
#include "filemanager_network_test.h"

#include <stdio.h>
#include <string.h>

#include "modules/filemanager/filemanager_network_internal.h"

/* ------------------------------------------------------------------------ */
/* selftest__run_filemanager_network_provider_parse_case                    */
/* ------------------------------------------------------------------------ */

bool selftest__run_filemanager_network_provider_parse_case(void) {
    char text[] =
        "sftp\n"
        "  ftp  \n"        /* leading/trailing spaces trimmed */
        "\n"               /* blank line skipped */
        "# a comment\n"
        "   \n"            /* whitespace-only line skipped */
        "webdav\r\n"       /* trailing \r trimmed (as if read from a CRLF file) */
        "this-provider-name-is-far-too-long-to-fit-in-the-buffer\n" /* oversized, skipped */
        "smb\n";
    char providers[8][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];
    size_t count = 0;
    filemanager__network_parse_providers(text, providers, 8, &count);

    bool basic_ok = count == 4 && strcmp(providers[0], "sftp") == 0 && strcmp(providers[1], "ftp") == 0 &&
                    strcmp(providers[2], "webdav") == 0 && strcmp(providers[3], "smb") == 0;

    /* max_providers caps how many are collected, even if more lines follow. */
    char text2[] = "one\ntwo\nthree\nfour\n";
    char capped[2][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];
    size_t capped_count = 0;
    filemanager__network_parse_providers(text2, capped, 2, &capped_count);
    bool cap_ok = capped_count == 2 && strcmp(capped[0], "one") == 0 && strcmp(capped[1], "two") == 0;

    /* Empty input yields zero providers, not a stale/garbage count. */
    char empty[] = "";
    char none[1][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];
    size_t none_count = 123;
    filemanager__network_parse_providers(empty, none, 1, &none_count);
    bool empty_ok = none_count == 0;

    bool ok = basic_ok && cap_ok && empty_ok;
    printf(
        "[selftest] filemanager/network-provider-parse: %s (basic=%d cap=%d empty=%d, count=%u)\n",
        ok ? "OK" : "FAIL", basic_ok, cap_ok, empty_ok, (unsigned)count
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_filemanager_network_sanitize_name_case                     */
/* ------------------------------------------------------------------------ */

static bool filemanager_network_test__sanitize(const char *name, const char *expected) {
    char out[64];
    filemanager__network_sanitize_name(name, out, sizeof(out));
    if (strcmp(out, expected) != 0) {
        printf(
            "[selftest] filemanager/network-sanitize-name: FAIL, sanitize(%s) = %s, expected %s\n", name, out,
            expected
        );
        return false;
    }
    return true;
}

bool selftest__run_filemanager_network_sanitize_name_case(void) {
    bool ok = true;
    ok &= filemanager_network_test__sanitize("myhost", "myhost");
    ok &= filemanager_network_test__sanitize("a/b\\c", "a_b_c");
    ok &= filemanager_network_test__sanitize("../../etc/passwd", ".._.._etc_passwd");
    ok &= filemanager_network_test__sanitize("", "");

    /* Output truncates at capacity rather than overrunning the buffer. */
    char small[4];
    filemanager__network_sanitize_name("toolongname", small, sizeof(small));
    bool truncated_ok = strlen(small) == 3 && strncmp(small, "too", 3) == 0;
    ok &= truncated_ok;

    printf("[selftest] filemanager/network-sanitize-name: %s (truncated=%d)\n", ok ? "OK" : "FAIL", truncated_ok);
    return ok;
}
