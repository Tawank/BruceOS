/* Pure-logic unit coverage for modules/ssh/ssh_sftp_app.c's "/.ssh/config"
 * host-pattern matching and known_hosts TOFU wire format -- no SSH/SFTP
 * connection, no network, no storage I/O. See ssh_sftp_internal.h for why
 * these are reachable from here at all (they're ssh_sftp_app.c's own
 * near-duplicates of ssh_app.c's equivalents, exposed there for exactly this
 * purpose). */
#include "ssh_sftp_test.h"

#include <stdio.h>
#include <string.h>

#include "modules/ssh/ssh_sftp_internal.h"

/* ------------------------------------------------------------------------ */
/* selftest__run_sftp_host_pattern_case                                     */
/* ------------------------------------------------------------------------ */

static bool sftp_test__pattern(const char *pattern, const char *host, bool expected) {
    bool actual = sftp_app__host_pattern_matches(pattern, host);
    if (actual != expected) {
        printf(
            "[selftest] sftp/host-pattern: FAIL, matches(%s, %s) = %d, expected %d\n", pattern, host, actual,
            expected
        );
        return false;
    }
    return true;
}

static bool sftp_test__list(const char *patterns, const char *host, bool expected) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s", patterns);
    bool actual = sftp_app__host_list_matches(buffer, host);
    if (actual != expected) {
        printf(
            "[selftest] sftp/host-pattern: FAIL, list_matches(%s, %s) = %d, expected %d\n", patterns, host, actual,
            expected
        );
        return false;
    }
    return true;
}

static bool sftp_test__literal(const char *token, bool expected) {
    bool actual = sftp_app__is_literal_host_token(token);
    if (actual != expected) {
        printf(
            "[selftest] sftp/host-pattern: FAIL, is_literal(%s) = %d, expected %d\n", token, actual, expected
        );
        return false;
    }
    return true;
}

bool selftest__run_sftp_host_pattern_case(void) {
    bool ok = true;

    /* '*' wildcard, '?' single-char wildcard, case-insensitivity. */
    ok &= sftp_test__pattern("*.example.com", "foo.example.com", true);
    ok &= sftp_test__pattern("*.example.com", "example.com", false);
    ok &= sftp_test__pattern("*", "anything.at.all", true);
    ok &= sftp_test__pattern("host?", "host1", true);
    ok &= sftp_test__pattern("host?", "host12", false);
    ok &= sftp_test__pattern("HOST.Example.COM", "host.example.com", true);
    ok &= sftp_test__pattern("host1", "host2", false);

    /* Space/tab-separated pattern lists, "!"-negation (a later negated
     * match excludes the host even though an earlier positive pattern
     * matched it -- same precedence as ssh_config's own Host directive). */
    ok &= sftp_test__list("web* !web3", "web1", true);
    ok &= sftp_test__list("web* !web3", "web3", false);
    ok &= sftp_test__list("alpha beta", "beta", true);
    ok &= sftp_test__list("alpha beta", "gamma", false);
    ok &= sftp_test__list("", "anything", false);

    /* Autodiscover only offers literal (non-wildcard, non-negated) Host
     * tokens as concrete connection targets. */
    ok &= sftp_test__literal("myhost", true);
    ok &= sftp_test__literal("my-host.local", true);
    ok &= sftp_test__literal("*.example.com", false);
    ok &= sftp_test__literal("host?", false);
    ok &= sftp_test__literal("!myhost", false);
    ok &= sftp_test__literal("", false);

    printf("[selftest] sftp/host-pattern: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_sftp_config_value_case                                     */
/* ------------------------------------------------------------------------ */

bool selftest__run_sftp_config_value_case(void) {
    char scratch[] = "  \t hello world \t\n ";
    char *trimmed = sftp_app__trim(scratch);
    bool trim_ok = strcmp(trimmed, "hello world") == 0;

    char empty[] = "   \t  ";
    bool trim_empty_ok = strcmp(sftp_app__trim(empty), "") == 0;

    /* First-match-wins: a second value is silently ignored once *was_set
     * is already true (mirrors "/.ssh/config"'s own first-block-wins
     * resolution rule). */
    char out[16] = {0};
    bool was_set = false;
    sftp_app__copy_config_value(out, sizeof(out), "first", &was_set);
    bool first_ok = was_set && strcmp(out, "first") == 0;
    sftp_app__copy_config_value(out, sizeof(out), "second", &was_set);
    bool second_ignored_ok = strcmp(out, "first") == 0;

    /* A value that doesn't fit isn't marked set at all -- the caller falls
     * back to whatever default it would have used had the field been
     * absent, rather than silently truncating a hostname/path. */
    char small[4] = {0};
    bool overflow_was_set = false;
    sftp_app__copy_config_value(small, sizeof(small), "toolong", &overflow_was_set);
    bool overflow_ok = !overflow_was_set;

    bool ok = trim_ok && trim_empty_ok && first_ok && second_ignored_ok && overflow_ok;
    printf(
        "[selftest] sftp/config-value: %s (trim=%d first=%d second_ignored=%d overflow=%d)\n", ok ? "OK" : "FAIL",
        trim_ok, first_ok, second_ignored_ok, overflow_ok
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_sftp_port_case                                             */
/* ------------------------------------------------------------------------ */

static bool sftp_test__port(const char *text, bool expect_ok, uint16_t expect_value) {
    uint16_t port = 0;
    bool actual = sftp_app__parse_port(text, &port);
    bool ok = actual == expect_ok && (!expect_ok || port == expect_value);
    if (!ok) {
        printf(
            "[selftest] sftp/port: FAIL, parse_port(%s) = (%d, %u), expected (%d, %u)\n", text ? text : "(null)",
            actual, (unsigned)port, expect_ok, (unsigned)expect_value
        );
    }
    return ok;
}

bool selftest__run_sftp_port_case(void) {
    bool ok = true;
    ok &= sftp_test__port("22", true, 22);
    ok &= sftp_test__port("65535", true, 65535);
    ok &= sftp_test__port("1", true, 1);
    ok &= sftp_test__port("0", false, 0);          /* port 0 is never valid */
    ok &= sftp_test__port("65536", false, 0);       /* overflows uint16_t */
    ok &= sftp_test__port("99999999", false, 0);    /* well past uint16_t */
    ok &= sftp_test__port("abc", false, 0);         /* not numeric at all */
    ok &= sftp_test__port("22abc", false, 0);       /* trailing garbage */
    ok &= sftp_test__port("", false, 0);
    ok &= sftp_test__port(NULL, false, 0);

    printf("[selftest] sftp/port: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_sftp_known_hosts_format_case                               */
/* ------------------------------------------------------------------------ */

bool selftest__run_sftp_known_hosts_format_case(void) {
    /* hex_encode/hex_decode round trip over a full SHA-256-sized digest. */
    uint8_t fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    for (size_t i = 0; i < sizeof(fingerprint); ++i) fingerprint[i] = (uint8_t)(i * 7 + 1);
    char hex[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    sftp_app__hex_encode(fingerprint, sizeof(fingerprint), hex);
    bool hex_len_ok = strlen(hex) == BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2;

    uint8_t roundtrip[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bool decode_ok =
        sftp_app__hex_decode(hex, strlen(hex), roundtrip, sizeof(roundtrip)) &&
        memcmp(fingerprint, roundtrip, sizeof(fingerprint)) == 0;

    bool hex_value_ok = sftp_app__hex_value('0') == 0 && sftp_app__hex_value('9') == 9 &&
                         sftp_app__hex_value('a') == 10 && sftp_app__hex_value('f') == 15 &&
                         sftp_app__hex_value('A') == 10 && sftp_app__hex_value('F') == 15 &&
                         sftp_app__hex_value('g') == -1 && sftp_app__hex_value(' ') == -1;

    /* Wrong length and invalid characters are both rejected, not silently
     * truncated/garbage-filled. */
    uint8_t discard[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bool wrong_length_rejected = !sftp_app__hex_decode(hex, strlen(hex) - 2, discard, sizeof(discard));
    char bad_hex[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    snprintf(bad_hex, sizeof(bad_hex), "%s", hex);
    bad_hex[0] = 'z';
    bool bad_char_rejected = !sftp_app__hex_decode(bad_hex, strlen(bad_hex), discard, sizeof(discard));

    /* find_known_fingerprint against a synthetic "<host>|<port> <hex>\n"
     * buffer -- the exact wire format sftp_app__store_known_fingerprint()
     * writes and ssh_app.c's own client reads. */
    uint8_t fp_a[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    uint8_t fp_b[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    for (size_t i = 0; i < sizeof(fp_a); ++i) fp_a[i] = (uint8_t)(i + 1);
    for (size_t i = 0; i < sizeof(fp_b); ++i) fp_b[i] = (uint8_t)(255 - i);
    char hex_a[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    char hex_b[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    sftp_app__hex_encode(fp_a, sizeof(fp_a), hex_a);
    sftp_app__hex_encode(fp_b, sizeof(fp_b), hex_b);

    char buffer[512];
    int written = snprintf(
        buffer, sizeof(buffer), "host1|22 %s\nhost2|2222 %s\nhost3|22 tooshorthex\n", hex_a, hex_b
    );
    size_t buffer_size = (size_t)written;

    uint8_t found[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bool find_a_ok = sftp_app__find_known_fingerprint(buffer, buffer_size, "host1|22", found) &&
                      memcmp(found, fp_a, sizeof(fp_a)) == 0;
    bool find_b_ok = sftp_app__find_known_fingerprint(buffer, buffer_size, "host2|2222", found) &&
                      memcmp(found, fp_b, sizeof(fp_b)) == 0;
    bool missing_ok = !sftp_app__find_known_fingerprint(buffer, buffer_size, "host-not-there|22", found);
    /* Different port on an otherwise-known host is a different known_hosts
     * key entirely -- deliberately not found. */
    bool wrong_port_ok = !sftp_app__find_known_fingerprint(buffer, buffer_size, "host1|2222", found);
    bool malformed_line_ok = !sftp_app__find_known_fingerprint(buffer, buffer_size, "host3|22", found);

    bool ok = hex_len_ok && decode_ok && hex_value_ok && wrong_length_rejected && bad_char_rejected && find_a_ok &&
              find_b_ok && missing_ok && wrong_port_ok && malformed_line_ok;
    printf(
        "[selftest] sftp/known-hosts-format: %s (roundtrip=%d find_a=%d find_b=%d missing=%d wrong_port=%d "
        "malformed=%d)\n",
        ok ? "OK" : "FAIL", decode_ok, find_a_ok, find_b_ok, missing_ok, wrong_port_ok, malformed_line_ok
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_sftp_split_directive_case                                  */
/* ------------------------------------------------------------------------ */

/* Regression coverage for a real bug: sftp_app__list_autodiscover() used to
 * carry its own copy of this splitting logic and compared `line` against
 * "Host" *before* null-terminating it at the split point, so it was really
 * comparing the whole "Host dsa" line against "Host" -- never equal, so no
 * configured host was ever discovered, only the "New connection..."
 * placeholder. Both callers now share sftp_app__split_directive(); this
 * pins down the exact contract that bug violated: `line` must already equal
 * just the keyword by the time a directive comparison runs against it. */
bool selftest__run_sftp_split_directive_case(void) {
    char basic[] = "Host dsa";
    char *value = NULL;
    bool basic_ok =
        sftp_app__split_directive(basic, &value) && strcmp(basic, "Host") == 0 && strcmp(value, "dsa") == 0;

    /* "=" is an accepted separator too, with surrounding whitespace trimmed
     * off the value on either side of it. */
    char equals[] = "HostName = 1.2.3.4 ";
    value = NULL;
    bool equals_ok = sftp_app__split_directive(equals, &value) && strcmp(equals, "HostName") == 0 &&
                      strcmp(value, "1.2.3.4") == 0;

    /* A bare keyword with no value at all, or an empty line, is rejected --
     * `line` is left untouched (not something a caller could mistake for a
     * successfully split "no value" directive). */
    char bare[] = "Host";
    value = NULL;
    bool bare_rejected = !sftp_app__split_directive(bare, &value) && strcmp(bare, "Host") == 0;
    char blank[] = "";
    value = NULL;
    bool blank_rejected = !sftp_app__split_directive(blank, &value);

    /* Multiple space-separated tokens in the value (e.g. several Host
     * aliases on one line) all survive as one still-split value string --
     * splitting the tokens apart is the caller's job, not this function's. */
    char multi[] = "Host dsa vps-alt";
    value = NULL;
    bool multi_ok =
        sftp_app__split_directive(multi, &value) && strcmp(multi, "Host") == 0 && strcmp(value, "dsa vps-alt") == 0;

    bool ok = basic_ok && equals_ok && bare_rejected && blank_rejected && multi_ok;
    printf(
        "[selftest] sftp/split-directive: %s (basic=%d equals=%d bare_rejected=%d blank_rejected=%d multi=%d)\n",
        ok ? "OK" : "FAIL", basic_ok, equals_ok, bare_rejected, blank_rejected, multi_ok
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_sftp_resolve_identity_path_case                            */
/* ------------------------------------------------------------------------ */

/* Regression coverage for a real bug: sftp_app__connect() used to pass an
 * "/.ssh/config" IdentityFile value straight to storage__open() with no
 * "~/" handling and no absolute-path check, unlike ssh_app.c's identical
 * config lookup -- so a value like "~/.ssh/id_rsa" that `ssh <alias>`
 * resolved and connected with just fine made `sftp <alias>`'s "device has no
 * home directory" storage layer fail with a bare BRUCE_ERR_INVALID_PATH,
 * surfaced as an unhelpful "authentication failed (-12)". */
bool selftest__run_sftp_resolve_identity_path_case(void) {
    const char *path = NULL;

    /* A leading "~/" is stripped down to just "/" (there's no home
     * directory here, so "~" always resolves to the storage root). */
    bool tilde_ok =
        sftp_app__resolve_identity_path("~/.ssh/id_rsa", &path) && strcmp(path, "/.ssh/id_rsa") == 0;

    /* Already-absolute paths (the common case: no IdentityFile at all, so
     * this is one of the built-in default identity paths) pass through
     * untouched. */
    path = NULL;
    bool absolute_ok =
        sftp_app__resolve_identity_path("/.ssh/id_ecdsa", &path) && strcmp(path, "/.ssh/id_ecdsa") == 0;

    /* Anything else non-absolute -- a bare relative path, or "~user/..."
     * (a different user's home directory, meaningless here) -- is rejected
     * outright rather than handed to storage__open() to fail on later. */
    path = NULL;
    bool relative_rejected = !sftp_app__resolve_identity_path("id_rsa", &path) && path == NULL;
    path = NULL;
    bool other_user_rejected = !sftp_app__resolve_identity_path("~otheruser/.ssh/id_rsa", &path) && path == NULL;

    bool ok = tilde_ok && absolute_ok && relative_rejected && other_user_rejected;
    printf(
        "[selftest] sftp/resolve-identity-path: %s (tilde=%d absolute=%d relative_rejected=%d "
        "other_user_rejected=%d)\n",
        ok ? "OK" : "FAIL", tilde_ok, absolute_ok, relative_rejected, other_user_rejected
    );
    return ok;
}
