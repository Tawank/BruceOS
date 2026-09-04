/* Pure-logic unit coverage for filemanager_network.c's provider-registry
 * JSON parsing, display-name sanitizing, and "/Network" entry-name
 * splitting -- no storage I/O, no provider process spawned. See
 * filemanager_network_internal.h for why these are reachable from here
 * (they're the parsing helpers factored out of
 * filemanager_network__load_providers()/_write_location()/_resolve_program()
 * for exactly this purpose). */
#include "filemanager_network_test.h"

#include <stdio.h>
#include <string.h>

#include "modules/filemanager/filemanager_network_internal.h"

/* ------------------------------------------------------------------------ */
/* selftest__run_filemanager_network_provider_parse_case                    */
/* ------------------------------------------------------------------------ */

bool selftest__run_filemanager_network_provider_parse_case(void) {
    /* A bare-array config (what "/config/filemanager.conf" actually holds);
     * "discovery" supplied for "sftp", omitted for "webdav" (so it should
     * default to "<program> list --autodiscover"); "smb" gets its own
     * distinct program. */
    const char *text = "[\n"
                        "  {\"name\": \"sftp\", \"program\": \"sftp\", \"discovery\": \"sftp list --autodiscover\"},\n"
                        "  {\"name\": \"webdav\", \"program\": \"webdav\"},\n"
                        "  {\"name\": \"smb\", \"program\": \"smb-client\", \"discovery\": \"smbdiscover --list\"}\n"
                        "]\n";
    filemanager_network__provider_t providers[8];
    size_t count = 0;
    bool parsed = filemanager_network__parse_providers_json(text, providers, 8, &count);

    bool basic_ok = parsed && count == 3 && strcmp(providers[0].name, "sftp") == 0 &&
                    strcmp(providers[0].program, "sftp") == 0 &&
                    strcmp(providers[0].discovery, "sftp list --autodiscover") == 0 &&
                    strcmp(providers[1].name, "webdav") == 0 &&
                    strcmp(providers[1].discovery, "webdav list --autodiscover") == 0 &&
                    strcmp(providers[2].program, "smb-client") == 0 &&
                    strcmp(providers[2].discovery, "smbdiscover --list") == 0;

    /* A {"providers": [...]} wrapper is accepted too. */
    const char *wrapped = "{\"providers\": [{\"name\": \"ftp\", \"program\": \"ftp\"}]}";
    filemanager_network__provider_t wrapped_providers[4];
    size_t wrapped_count = 0;
    bool wrapped_ok = filemanager_network__parse_providers_json(wrapped, wrapped_providers, 4, &wrapped_count) &&
                       wrapped_count == 1 && strcmp(wrapped_providers[0].name, "ftp") == 0;

    /* An entry missing "name"/"program", or whose "name" has a space in it
     * (it wouldn't survive filemanager_network__split_entry_name() later),
     * is skipped rather than aborting the whole parse. */
    const char *partial = "[{\"program\": \"noname\"}, {\"name\": \"has space\", \"program\": \"x\"}, "
                           "{\"name\": \"ok\", \"program\": \"ok\"}]";
    filemanager_network__provider_t partial_providers[4];
    size_t partial_count = 0;
    bool partial_ok = filemanager_network__parse_providers_json(partial, partial_providers, 4, &partial_count) &&
                       partial_count == 1 && strcmp(partial_providers[0].name, "ok") == 0;

    /* max_providers caps how many are collected, even if more entries follow. */
    const char *many = "[{\"name\":\"a\",\"program\":\"a\"},{\"name\":\"b\",\"program\":\"b\"},"
                        "{\"name\":\"c\",\"program\":\"c\"}]";
    filemanager_network__provider_t capped[2];
    size_t capped_count = 0;
    bool cap_ok = filemanager_network__parse_providers_json(many, capped, 2, &capped_count) && capped_count == 2 &&
                  strcmp(capped[0].name, "a") == 0 && strcmp(capped[1].name, "b") == 0;

    /* An empty array yields zero providers (not an error), while text that
     * isn't a provider list at all (not JSON, or a JSON value that isn't an
     * array/{"providers":[...]} object) reports failure. */
    filemanager_network__provider_t none[1];
    size_t none_count = 123;
    bool empty_ok = filemanager_network__parse_providers_json("[]", none, 1, &none_count) && none_count == 0;
    size_t garbage_count = 0;
    bool garbage_ok = !filemanager_network__parse_providers_json("not json", none, 1, &garbage_count) &&
                      garbage_count == 0;

    bool ok = basic_ok && wrapped_ok && partial_ok && cap_ok && empty_ok && garbage_ok;
    printf(
        "[selftest] filemanager/network-provider-parse: %s (basic=%d wrapped=%d partial=%d cap=%d empty=%d "
        "garbage=%d)\n",
        ok ? "OK" : "FAIL", basic_ok, wrapped_ok, partial_ok, cap_ok, empty_ok, garbage_ok
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_filemanager_network_sanitize_name_case                     */
/* ------------------------------------------------------------------------ */

static bool filemanager_network_test__sanitize(const char *name, const char *expected) {
    char out[64];
    filemanager_network__sanitize_name(name, out, sizeof(out));
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
    filemanager_network__sanitize_name("toolongname", small, sizeof(small));
    bool truncated_ok = strlen(small) == 3 && strncmp(small, "too", 3) == 0;
    ok &= truncated_ok;

    printf("[selftest] filemanager/network-sanitize-name: %s (truncated=%d)\n", ok ? "OK" : "FAIL", truncated_ok);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_filemanager_network_split_entry_name_case                  */
/* ------------------------------------------------------------------------ */

bool selftest__run_filemanager_network_split_entry_name_case(void) {
    char name[32];

    /* Only the *first* space is the boundary -- a label may contain its own
     * spaces (e.g. sftp's "New connection..." entry). */
    bool basic_ok = filemanager_network__split_entry_name("sftp New connection...", name, sizeof(name)) &&
                    strcmp(name, "sftp") == 0;
    bool label_only_space_ok =
        filemanager_network__split_entry_name("sftp myVps", name, sizeof(name)) && strcmp(name, "sftp") == 0;

    /* No space at all -- not shaped like a provider-owned entry. */
    bool no_space_rejected = !filemanager_network__split_entry_name("stray-file.txt", name, sizeof(name));
    /* A leading space would yield an empty provider name -- also rejected. */
    bool leading_space_rejected = !filemanager_network__split_entry_name(" leading", name, sizeof(name));

    /* Output buffer too small for the provider name fails cleanly. */
    char tiny[3];
    bool too_small_rejected = !filemanager_network__split_entry_name("sftp myVps", tiny, sizeof(tiny));

    bool ok = basic_ok && label_only_space_ok && no_space_rejected && leading_space_rejected && too_small_rejected;
    printf(
        "[selftest] filemanager/network-split-entry-name: %s (basic=%d label_space=%d no_space=%d leading=%d "
        "too_small=%d)\n",
        ok ? "OK" : "FAIL", basic_ok, label_only_space_ok, no_space_rejected, leading_space_rejected,
        too_small_rejected
    );
    return ok;
}
