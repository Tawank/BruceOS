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
