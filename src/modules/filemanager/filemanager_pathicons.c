#include "filemanager_pathicons.h"
#include "filemanager_pathicons_internal.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "core_sdk/app_config.h"
#include "core_sdk/memory.h"

#include "filemanager_network.h" /* FILEMANAGER_NETWORK_DIR */

#define FILEMANAGER_PATHICONS_APP_NAME "filemanager"
#define FILEMANAGER_PATHICONS_MAX_ENTRIES 8
#define FILEMANAGER_PATHICONS_JSON_MAX 512u

static const char *const FILEMANAGER_PATHICONS_DEFAULT_JSON =
    "[{\"path\": \"" FILEMANAGER_NETWORK_DIR "\", \"icon\": \"server\"}]";

bool filemanager_pathicons__parse_json(
    const char *json_text, filemanager_pathicons__entry_t *entries, size_t max_entries, size_t *out_count
) {
    *out_count = 0;
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) return false;
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        if (*out_count >= max_entries) break;
        if (!cJSON_IsObject(entry)) continue;

        cJSON *path = cJSON_GetObjectItemCaseSensitive(entry, "path");
        cJSON *icon = cJSON_GetObjectItemCaseSensitive(entry, "icon");
        if (!cJSON_IsString(path) || path->valuestring == NULL || path->valuestring[0] == '\0') continue;
        if (!cJSON_IsString(icon) || icon->valuestring == NULL || icon->valuestring[0] == '\0') continue;

        filemanager_pathicons__entry_t *out = &entries[*out_count];
        if (strlen(path->valuestring) >= sizeof(out->path)) continue;
        if (strlen(icon->valuestring) >= sizeof(out->icon)) continue;
        snprintf(out->path, sizeof(out->path), "%s", path->valuestring);
        snprintf(out->icon, sizeof(out->icon), "%s", icon->valuestring);
        ++*out_count;
    }
    cJSON_Delete(root);
    return true;
}

bool filemanager_pathicons__match(
    const filemanager_pathicons__entry_t *entries, size_t entry_count, const char *path, char *out_icon,
    size_t out_icon_size
) {
    for (size_t i = 0; i < entry_count; ++i) {
        if (strcmp(entries[i].path, path) == 0) {
            snprintf(out_icon, out_icon_size, "%s", entries[i].icon);
            return true;
        }
    }
    return false;
}

/* Reads "/config/filemanager.conf"'s "pathicons" array via app_config,
 * seeding it with FILEMANAGER_PATHICONS_DEFAULT_JSON the first time (i.e.
 * whenever the app has no config file yet, or has one but no "pathicons"
 * key in it) so the value is visible and hand-editable there afterward.
 * Falls back to that same default (without writing it) if the stored value
 * exists but fails to parse, so a hand-edited syntax error degrades to "as
 * if unconfigured" instead of leaving every path icon silently gone. */
static size_t filemanager_pathicons__load(filemanager_pathicons__entry_t entries[], size_t max_entries) {
    char *json = memory__malloc(FILEMANAGER_PATHICONS_JSON_MAX);
    if (json == NULL) return 0;
    bool configured = app_config__get_json(
        FILEMANAGER_PATHICONS_APP_NAME, "pathicons", FILEMANAGER_PATHICONS_DEFAULT_JSON, json,
        FILEMANAGER_PATHICONS_JSON_MAX
    );
    if (!configured) {
        (void)app_config__set_json(
            FILEMANAGER_PATHICONS_APP_NAME, "pathicons", FILEMANAGER_PATHICONS_DEFAULT_JSON
        );
    }

    size_t count = 0;
    if (!filemanager_pathicons__parse_json(json, entries, max_entries, &count)) {
        (void)filemanager_pathicons__parse_json(FILEMANAGER_PATHICONS_DEFAULT_JSON, entries, max_entries, &count);
    }
    memory__free(json);
    return count;
}

bool filemanager_pathicons__icon_for_path(
    const char *path, bool is_directory, char *out_icon, size_t out_icon_size, void *context
) {
    (void)is_directory;
    (void)context;
    filemanager_pathicons__entry_t *entries =
        memory__malloc(FILEMANAGER_PATHICONS_MAX_ENTRIES * sizeof(filemanager_pathicons__entry_t));
    if (entries == NULL) return false;
    size_t count = filemanager_pathicons__load(entries, FILEMANAGER_PATHICONS_MAX_ENTRIES);
    bool found = filemanager_pathicons__match(entries, count, path, out_icon, out_icon_size);
    memory__free(entries);
    return found;
}
