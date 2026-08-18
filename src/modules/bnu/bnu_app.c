#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/environment.h"
#include "core_sdk/format.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"

/*
 * Helpers shared by the bnu_*_app.c command implementations. Each file
 * groups a family of built-in commands (filesystem, disk, system, network);
 * this file holds only the pieces common to all of them.
 */

#define BNU__PWD_NAME "PWD"

const char *bnu__get_working_directory(void) {
    const char *value = environment__get(BNU__PWD_NAME);
    return value != NULL && value[0] == '/' ? value : "/";
}

int bnu__parse_failure(ArgParser *parser) {
    ap_status_t status = ap_get_status(parser);
    ap_free(parser);
    if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
    return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
}

ArgParser *bnu__new_parser(const char *helptext) {
    ArgParser *parser = ap_new_parser();
    if (parser != NULL) ap_set_helptext(parser, helptext);
    return parser;
}

bool bnu__resolve_path(const char *path, char *out_path) {
    char combined[BRUCE_STORAGE_PATH_MAX * 2];
    const char *working_directory = bnu__get_working_directory();
    if (path == NULL || path[0] == '\0') path = working_directory;
    int written = path[0] == '/' ? snprintf(combined, sizeof(combined), "%s", path)
                                 : snprintf(
                                       combined,
                                       sizeof(combined),
                                       "%s%s%s",
                                       working_directory,
                                       strcmp(working_directory, "/") == 0 ? "" : "/",
                                       path
                                   );
    if (written < 0 || (size_t)written >= sizeof(combined)) return false;

    size_t out_length = 1;
    out_path[0] = '/';
    out_path[1] = '\0';
    const char *cursor = combined;
    while (*cursor != '\0') {
        while (*cursor == '/') cursor++;
        const char *component = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - component);
        if (length == 0 || (length == 1 && component[0] == '.')) continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            while (out_length > 1 && out_path[out_length - 1] != '/') out_length--;
            if (out_length > 1) out_length--;
            out_path[out_length] = '\0';
            continue;
        }
        size_t separator = out_length > 1 ? 1u : 0u;
        if (out_length + separator + length >= BRUCE_STORAGE_PATH_MAX) return false;
        if (separator != 0) out_path[out_length++] = '/';
        memcpy(out_path + out_length, component, length);
        out_length += length;
        out_path[out_length] = '\0';
    }
    return true;
}

/* -h is reserved by ArgParser for --help, so human-readable output uses -H
 * (matching du/df/ls's -h intent, just on a free letter). */
void bnu__format_size(uint32_t bytes, bool human, char *output, size_t capacity) {
    if (human) format__bytes_human(bytes, output, capacity);
    else snprintf(output, capacity, "%u", (unsigned)bytes);
}
