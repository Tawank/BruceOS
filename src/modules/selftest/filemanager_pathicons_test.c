/* Pure-logic unit coverage for filemanager_pathicons.c's "pathicons" JSON
 * parsing and path matching -- no app_config/storage I/O. See
 * filemanager_pathicons_internal.h for why these are reachable from here
 * (they're the parsing/matching helpers factored out of
 * filemanager_pathicons__icon_for_path() for exactly this purpose). */
#include "filemanager_pathicons_test.h"

#include <stdio.h>
#include <string.h>

#include "modules/filemanager/filemanager_pathicons_internal.h"

/* ------------------------------------------------------------------------ */
/* selftest__run_filemanager_pathicons_parse_case                           */
/* ------------------------------------------------------------------------ */

bool selftest__run_filemanager_pathicons_parse_case(void) {
    const char *text = "[\n"
                        "  {\"path\": \"/Network\", \"icon\": \"server\"},\n"
                        "  {\"path\": \"/roms\", \"icon\": \"gamepad\"}\n"
                        "]\n";
    filemanager_pathicons__entry_t entries[8];
    size_t count = 0;
    bool parsed = filemanager_pathicons__parse_json(text, entries, 8, &count);
    bool basic_ok = parsed && count == 2 && strcmp(entries[0].path, "/Network") == 0 &&
                    strcmp(entries[0].icon, "server") == 0 && strcmp(entries[1].path, "/roms") == 0 &&
                    strcmp(entries[1].icon, "gamepad") == 0;

    /* An entry missing "path"/"icon", or with either empty, is skipped
     * rather than aborting the whole parse. */
    const char *partial = "[{\"icon\": \"noicon\"}, {\"path\": \"/nopath\"}, "
                           "{\"path\": \"\", \"icon\": \"x\"}, {\"path\": \"/ok\", \"icon\": \"ok\"}]";
    filemanager_pathicons__entry_t partial_entries[4];
    size_t partial_count = 0;
    bool partial_ok = filemanager_pathicons__parse_json(partial, partial_entries, 4, &partial_count) &&
                      partial_count == 1 && strcmp(partial_entries[0].path, "/ok") == 0;

    /* max_entries caps how many are collected, even if more entries follow. */
    const char *many = "[{\"path\":\"/a\",\"icon\":\"a\"},{\"path\":\"/b\",\"icon\":\"b\"},"
                        "{\"path\":\"/c\",\"icon\":\"c\"}]";
    filemanager_pathicons__entry_t capped[2];
    size_t capped_count = 0;
    bool cap_ok = filemanager_pathicons__parse_json(many, capped, 2, &capped_count) && capped_count == 2 &&
                  strcmp(capped[0].path, "/a") == 0 && strcmp(capped[1].path, "/b") == 0;

    /* An empty array yields zero entries (not an error), while text that
     * isn't a JSON array at all reports failure. */
    filemanager_pathicons__entry_t none[1];
    size_t none_count = 123;
    bool empty_ok = filemanager_pathicons__parse_json("[]", none, 1, &none_count) && none_count == 0;
    size_t garbage_count = 0;
    bool garbage_ok = !filemanager_pathicons__parse_json("not json", none, 1, &garbage_count) &&
                      garbage_count == 0;

    bool ok = basic_ok && partial_ok && cap_ok && empty_ok && garbage_ok;
    printf(
        "[selftest] filemanager/pathicons-parse: %s (basic=%d partial=%d cap=%d empty=%d garbage=%d)\n",
        ok ? "OK" : "FAIL", basic_ok, partial_ok, cap_ok, empty_ok, garbage_ok
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_filemanager_pathicons_match_case                           */
/* ------------------------------------------------------------------------ */

bool selftest__run_filemanager_pathicons_match_case(void) {
    filemanager_pathicons__entry_t entries[2];
    snprintf(entries[0].path, sizeof(entries[0].path), "/Network");
    snprintf(entries[0].icon, sizeof(entries[0].icon), "server");
    snprintf(entries[1].path, sizeof(entries[1].path), "/roms");
    snprintf(entries[1].icon, sizeof(entries[1].icon), "gamepad");

    char out[32];
    bool hit_ok = filemanager_pathicons__match(entries, 2, "/Network", out, sizeof(out)) &&
                  strcmp(out, "server") == 0;

    /* Matching is exact, not a prefix -- "/Network/host.sftp" (an entry
     * *inside* "/Network") doesn't match the "/Network" pathicon. */
    bool prefix_rejected = !filemanager_pathicons__match(entries, 2, "/Network/host.sftp", out, sizeof(out));
    bool miss_rejected = !filemanager_pathicons__match(entries, 2, "/roms2", out, sizeof(out));

    bool ok = hit_ok && prefix_rejected && miss_rejected;
    printf(
        "[selftest] filemanager/pathicons-match: %s (hit=%d prefix_rejected=%d miss_rejected=%d)\n",
        ok ? "OK" : "FAIL", hit_ok, prefix_rejected, miss_rejected
    );
    return ok;
}
