#include "bnu_sed_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core/storage/storage.h"
#include "modules/bnu/bnu_app.h"

/* Runs `argv`/`argc` through bnu_sed_app_main(), capturing its stdio__write()
 * output the same way selftest__run_man_builtin_case() captures a direct,
 * same-task call's output. stdio__write() auto-inserts '\r' before every
 * '\n' (ONLCR, for the terminal grid) -- dropped here so callers can compare
 * against plain '\n'-terminated expected text. Copies at most `capacity - 1`
 * bytes into `text` and NUL-terminates. */
static bruce_result_t
selftest__sed_capture(int argc, char **argv, char *text, size_t capacity, int *out_status) {
    bruce_stdio_session_t capture = BRUCE_STDIO_SESSION_INVALID;
    bruce_result_t result = stdio__session_create(&capture);
    if (result == BRUCE_OK) result = stdio__session_capture_self(capture);
    if (result != BRUCE_OK) {
        if (capture != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(capture);
        return result;
    }

    *out_status = bnu_sed_app_main(argc, argv);
    (void)stdio__session_release_self();

    size_t total = 0;
    for (;;) {
        char chunk[128];
        size_t size = 0;
        if (stdio__session_read_output(capture, chunk, sizeof(chunk), &size) != BRUCE_OK || size == 0) break;
        for (size_t i = 0; i < size && total < capacity - 1; ++i) {
            if (chunk[i] != '\r') text[total++] = chunk[i];
        }
    }
    text[total] = '\0';
    (void)stdio__session_close(capture);
    return BRUCE_OK;
}

static bool selftest__sed_check(
    bool ok, const char *label, int status, int want_status, const char *text, const char *want_text
) {
    bool pass = status == want_status && strcmp(text, want_text) == 0;
    if (!pass) printf("[selftest] bnu/sed: %s failed (status=%d text=%s)\n", label, status, text);
    return ok && pass;
}

bool selftest__run_bnu_sed_case(void) {
    bool ok = true;

    static const char fixture_text[] =
        "alpha bar bar\nBETA bar\ngamma\nzeta 123 abc\nroot:/home/user\n";
    static const char empty_match_text[] = "ab\n";
    const char *fixture_path = "/selftest_bnu_sed.txt";
    const char *empty_match_path = "/selftest_bnu_sed_empty.txt";

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    bruce_result_t setup = storage__open(
        fixture_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (setup == BRUCE_OK) setup = storage__write(file, fixture_text, sizeof(fixture_text) - 1, &written);
    if (setup == BRUCE_OK) setup = storage__close(file);
    if (setup == BRUCE_OK) {
        setup = storage__open(
            empty_match_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
            &file
        );
    }
    if (setup == BRUCE_OK) setup = storage__write(file, empty_match_text, sizeof(empty_match_text) - 1, &written);
    if (setup == BRUCE_OK) setup = storage__close(file);
    if (setup != BRUCE_OK) {
        printf("[selftest] bnu/sed: fixture setup failed (result=%d)\n", setup);
        return false;
    }

    char text[256];
    int status = 0;

    char *substitute_argv[] = {"sed", "s/bar/BAZ/", (char *)fixture_path};
    (void)selftest__sed_capture(3, substitute_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(
        ok, "s/// (first match only)", status, BRUCE_OK, text,
        "alpha BAZ bar\nBETA BAZ\ngamma\nzeta 123 abc\nroot:/home/user\n"
    );

    char *global_argv[] = {"sed", "s/bar/BAZ/g", (char *)fixture_path};
    (void)selftest__sed_capture(3, global_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(
        ok, "s///g", status, BRUCE_OK, text,
        "alpha BAZ BAZ\nBETA BAZ\ngamma\nzeta 123 abc\nroot:/home/user\n"
    );

    char *ignore_case_argv[] = {"sed", "s/beta/X/i", (char *)fixture_path};
    (void)selftest__sed_capture(3, ignore_case_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(
        ok, "s///i", status, BRUCE_OK, text, "alpha bar bar\nX bar\ngamma\nzeta 123 abc\nroot:/home/user\n"
    );

    char *delim_argv[] = {"sed", "s|/home|/root|", (char *)fixture_path};
    (void)selftest__sed_capture(3, delim_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(
        ok, "s with '|' delimiter", status, BRUCE_OK, text,
        "alpha bar bar\nBETA bar\ngamma\nzeta 123 abc\nroot:/root/user\n"
    );

    char *backref_argv[] = {"sed", "s/([a-z]+) ([0-9]+)/\\2-\\1/", (char *)fixture_path};
    (void)selftest__sed_capture(3, backref_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(
        ok, "s/// backreference", status, BRUCE_OK, text,
        "alpha bar bar\nBETA bar\ngamma\n123-zeta abc\nroot:/home/user\n"
    );

    char *whole_match_argv[] = {"sed", "s/bar/[&]/g", (char *)fixture_path};
    (void)selftest__sed_capture(3, whole_match_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(
        ok, "s/// whole-match &", status, BRUCE_OK, text,
        "alpha [bar] [bar]\nBETA [bar]\ngamma\nzeta 123 abc\nroot:/home/user\n"
    );

    char *delete_argv[] = {"sed", "/gamma/d", (char *)fixture_path};
    (void)selftest__sed_capture(3, delete_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(
        ok, "/re/d", status, BRUCE_OK, text, "alpha bar bar\nBETA bar\nzeta 123 abc\nroot:/home/user\n"
    );

    char *print_argv[] = {"sed", "-n", "/bar/p", (char *)fixture_path};
    (void)selftest__sed_capture(4, print_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(ok, "-n /re/p", status, BRUCE_OK, text, "alpha bar bar\nBETA bar\n");

    char *empty_match_argv[] = {"sed", "s/x*/-/g", (char *)empty_match_path};
    (void)selftest__sed_capture(3, empty_match_argv, text, sizeof(text), &status);
    ok = selftest__sed_check(ok, "s/// empty match", status, BRUCE_OK, text, "-a-b-\n");

    char *invalid_argv[] = {"sed", "s/unterminated", (char *)fixture_path};
    int invalid_status = bnu_sed_app_main(3, invalid_argv);
    if (invalid_status != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] bnu/sed: invalid script failed (status=%d)\n", invalid_status);
        ok = false;
    }

    printf("[selftest] bnu/sed: %s\n", ok ? "OK" : "failed");
    return ok;
}
