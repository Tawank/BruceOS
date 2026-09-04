#include "filemanager_network.h"
#include "filemanager_network_internal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "core_sdk/app_config.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#include "embedded_resources.h"
#include "filemanager_internal.h"

/**
 * @name Network folder
 *
 * "/Network" is a plain directory, populated on entry with one plain file
 * per remote location a *provider* discovers -- filemanager itself has no
 * idea what SSH or SFTP are. Providers are configured under
 * "/config/filemanager.conf"'s "providers" key -- read/written through
 * core_sdk/app_config.h, addressed as app_name "filemanager" -- as a JSON
 * array of {"name", "program", "discovery"} objects, seeded from this
 * module's own embedded default (FILEMANAGER_NETWORK_PROVIDERS_DEFAULT_JSON
 * below) the first time that key is read -- the same "get, and set back if
 * absent" idiom modules/system_menu/system_menu_app.c uses for its own
 * "items" key, and modules/filemanager/filemanager_pathicons.c uses for the
 * sibling "pathicons" key -- e.g.:
 *
 *   [{"name": "sftp", "program": "sftp", "discovery": "sftp list --autodiscover"}]
 *
 * "discovery" is a full command line: run with no GUI and its stdout
 * captured, it prints one "<display name>\t<location>" line per location the
 * provider knows about (e.g. host aliases read from "/.ssh/config", plus a
 * "New connection..." line of the provider's own choosing). Each line
 * becomes "/Network/<display name>.<name>", containing just the location
 * string -- a real extension, not just a naming convention: opening that
 * file goes through filemanager_network__resolve_program() below, which
 * splits the trailing ".<name>" back off and looks it up against "program"
 * in the same config, but "<name>" doubling as the file's actual extension
 * means "/config/extensions.conf" can independently give it an icon (see
 * ".sftp"'s own entry there) and filemanager__open_default()'s ordinary
 * by-extension dispatch works as a fallback too -- so a provider still
 * needs no extensions.conf entry to *function* (resolve_program() below is
 * checked first and covers that on its own), only to look like something
 * other than a plain file in a listing. A user can drop their own
 * "myhost.sftp" file straight into "/Network" by hand the same way -- its
 * content is read exactly like a discovered one is (see
 * sftp_app__open_location() in modules/ssh/ssh_sftp_app.c).
 * @{
 */

#define FILEMANAGER_NETWORK_APP_NAME "filemanager"
#define FILEMANAGER_NETWORK_PROVIDERS_DEFAULT_JSON json_filemanager_json
#define FILEMANAGER_NETWORK_PROVIDER_MAX 8
#define FILEMANAGER_NETWORK_PROVIDERS_JSON_MAX_BYTES 2047u
#define FILEMANAGER_NETWORK_CAPTURE_MAX 4096
/* FILEMANAGER_NETWORK_PROVIDER_NAME_MAX/DISCOVERY_MAX, the provider struct,
 * and the pure parsing helpers now live in filemanager_network_internal.h --
 * selftest unit-tests those directly (see its header comment). */

bool filemanager_network__split_entry_name(const char *entry_name, char *name_out, size_t name_out_size) {
    const char *dot = strrchr(entry_name, '.');
    if (dot == NULL || dot[1] == '\0') return false;
    const char *provider_name = dot + 1;
    size_t len = strlen(provider_name);
    if (len >= name_out_size) return false;
    memcpy(name_out, provider_name, len);
    name_out[len] = '\0';
    return true;
}

void filemanager_network__sanitize_name(const char *name, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0' && j + 1 < out_size; ++i) {
        char c = name[i];
        out[j++] = (c == '/' || c == '\\') ? '_' : c;
    }
    out[j] = '\0';
}

bool filemanager_network__parse_providers_json(
    const char *json_text, filemanager_network__provider_t *providers, size_t max_providers, size_t *out_count
) {
    *out_count = 0;
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) return false;

    cJSON *array = root;
    if (!cJSON_IsArray(array)) array = cJSON_GetObjectItemCaseSensitive(root, "providers");
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        if (*out_count >= max_providers) break;
        if (!cJSON_IsObject(entry)) continue;

        cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        cJSON *program = cJSON_GetObjectItemCaseSensitive(entry, "program");
        if (!cJSON_IsString(name) || name->valuestring == NULL || name->valuestring[0] == '\0') continue;
        if (!cJSON_IsString(program) || program->valuestring == NULL || program->valuestring[0] == '\0') continue;
        /* Becomes the literal extension on every location file this provider
         * writes (see filemanager_network__write_location() below), so it
         * has to survive filemanager_network__split_entry_name() splitting
         * that back off the *last* '.' later -- a "." of its own would
         * shift where that split lands, and a space is just not a sane
         * extension character either. */
        if (strchr(name->valuestring, ' ') != NULL || strchr(name->valuestring, '.') != NULL) continue;
        if (strlen(name->valuestring) >= FILEMANAGER_NETWORK_PROVIDER_NAME_MAX) continue;
        if (strlen(program->valuestring) >= FILEMANAGER_NETWORK_PROVIDER_NAME_MAX) continue;

        filemanager_network__provider_t *out = &providers[*out_count];
        snprintf(out->name, sizeof(out->name), "%s", name->valuestring);
        snprintf(out->program, sizeof(out->program), "%s", program->valuestring);

        cJSON *discovery = cJSON_GetObjectItemCaseSensitive(entry, "discovery");
        if (cJSON_IsString(discovery) && discovery->valuestring != NULL && discovery->valuestring[0] != '\0') {
            snprintf(out->discovery, sizeof(out->discovery), "%s", discovery->valuestring);
        } else {
            snprintf(out->discovery, sizeof(out->discovery), "%s list --autodiscover", out->program);
        }
        ++*out_count;
    }
    cJSON_Delete(root);
    return true;
}

/* Reads "/config/filemanager.conf"'s "providers" key via app_config,
 * seeding it with FILEMANAGER_NETWORK_PROVIDERS_DEFAULT_JSON the first time
 * (i.e. whenever the app has no config file yet, or has one but no
 * "providers" key in it) so the value is visible and hand-editable there
 * afterward. Falls back to that same default (without writing it) if the
 * stored value exists but fails to parse, so a hand-edited config with a
 * syntax error degrades to "as if unconfigured" rather than leaving
 * "/Network" silently empty. */
static void filemanager_network__load_providers(
    filemanager_network__provider_t providers[], size_t max_providers, size_t *out_count
) {
    *out_count = 0;
    char *json = memory__malloc(FILEMANAGER_NETWORK_PROVIDERS_JSON_MAX_BYTES + 1u);
    if (json == NULL) return;
    bool configured = app_config__get_json(
        FILEMANAGER_NETWORK_APP_NAME, "providers", FILEMANAGER_NETWORK_PROVIDERS_DEFAULT_JSON, json,
        FILEMANAGER_NETWORK_PROVIDERS_JSON_MAX_BYTES + 1u
    );
    if (!configured) {
        (void)app_config__set_json(
            FILEMANAGER_NETWORK_APP_NAME, "providers", FILEMANAGER_NETWORK_PROVIDERS_DEFAULT_JSON
        );
    }

    if (!filemanager_network__parse_providers_json(json, providers, max_providers, out_count)) {
        (void)filemanager_network__parse_providers_json(
            FILEMANAGER_NETWORK_PROVIDERS_DEFAULT_JSON, providers, max_providers, out_count
        );
    }
    memory__free(json);
}

static const filemanager_network__provider_t *filemanager_network__find_provider(
    const filemanager_network__provider_t providers[], size_t provider_count, const char *name
) {
    for (size_t i = 0; i < provider_count; ++i) {
        if (strcmp(providers[i].name, name) == 0) return &providers[i];
    }
    return NULL;
}

bool filemanager_network__resolve_program(const char *path, char *program, size_t program_size) {
    static const char prefix[] = FILEMANAGER_NETWORK_DIR "/";
    size_t prefix_len = sizeof(prefix) - 1u;
    if (strncmp(path, prefix, prefix_len) != 0) return false;
    const char *entry_name = path + prefix_len;
    /* Must be a direct child, not something nested further under "/Network"
     * (that directory only ever holds discovered location files, but this
     * guards against a stray subfolder someone made by hand anyway). */
    if (strchr(entry_name, '/') != NULL) return false;

    char provider_name[FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];
    if (!filemanager_network__split_entry_name(entry_name, provider_name, sizeof(provider_name))) return false;

    /* Heap-allocated, not a stack array: this runs from filemanager__open_default(),
     * already several calls deep into the file-open dispatch chain, and
     * FILEMANAGER_NETWORK_PROVIDER_MAX providers worth of struct is too much to add
     * to that stack (see the identical reasoning on filemanager_network__refresh()'s
     * own copy of this array, and filemanager_network__clear_stale()'s entries[]). */
    filemanager_network__provider_t *providers =
        memory__malloc(FILEMANAGER_NETWORK_PROVIDER_MAX * sizeof(filemanager_network__provider_t));
    if (providers == NULL) return false;
    size_t provider_count = 0;
    filemanager_network__load_providers(providers, FILEMANAGER_NETWORK_PROVIDER_MAX, &provider_count);

    const filemanager_network__provider_t *provider =
        filemanager_network__find_provider(providers, provider_count, provider_name);
    bool found = provider != NULL;
    if (found) snprintf(program, program_size, "%s", provider->program);
    memory__free(providers);
    return found;
}

/* Runs a provider's "discovery" command line and captures its stdout via a
 * redirected stdio session -- the same trick the shell uses for "$(...)"
 * command substitution (see shell_executor__capture_external() in
 * modules/shell/shell_executor.c), built entirely on public core_sdk
 * primitives so this needs no new core plumbing. Output beyond out_capacity
 * is silently dropped rather than growing the buffer: a location listing is
 * expected to be short text, not arbitrary command output. */
static bruce_result_t
filemanager_network__capture(const filemanager_network__provider_t *provider, char *out, size_t out_capacity, size_t *out_size) {
    *out_size = 0;
    char command[FILEMANAGER_NETWORK_DISCOVERY_MAX];
    snprintf(command, sizeof(command), "%s", provider->discovery);
    char *args = strchr(command, ' ');
    if (args != NULL) {
        *args++ = '\0';
        while (*args == ' ') ++args;
    } else {
        args = command + strlen(command); /* empty */
    }
    const char *binary = command;
    if (binary[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK) return BRUCE_ERR_IO;
    if (stdio__session_route_children(session) != BRUCE_OK) {
        (void)stdio__session_close(session);
        return BRUCE_ERR_IO;
    }
    int process = app_runner__run(binary, args, BRUCE_LAUNCH_FOREGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (process <= 0) {
        (void)stdio__session_close(session);
        return BRUCE_ERR_NOT_FOUND;
    }

    size_t total = 0;
    bruce_process_status_t status = {0};
    bool complete = false;
    while (!complete) {
        char chunk[128];
        size_t size = 0;
        while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
            size_t copy = size < out_capacity - 1u - total ? size : out_capacity - 1u - total;
            if (copy > 0) memcpy(out + total, chunk, copy);
            total += copy;
        }
        bruce_result_t waited = process__wait_status((bruce_process_id_t)process, 0, &status);
        complete = waited == BRUCE_OK;
        if (!complete && waited != BRUCE_ERR_TIMEOUT) break;
        if (!complete) (void)runtime__delay(20);
    }
    char chunk[128];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        size_t copy = size < out_capacity - 1u - total ? size : out_capacity - 1u - total;
        if (copy > 0) memcpy(out + total, chunk, copy);
        total += copy;
    }
    (void)stdio__session_close(session);
    out[total] = '\0';
    *out_size = total;
    return BRUCE_OK;
}

static bruce_result_t filemanager_network__write_location(
    const filemanager_network__provider_t *provider, const char *display_name, const char *location
) {
    char safe_label[BRUCE_STORAGE_NAME_MAX];
    filemanager_network__sanitize_name(display_name, safe_label, sizeof(safe_label));
    if (safe_label[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;

    char path[BRUCE_STORAGE_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s.%s", FILEMANAGER_NETWORK_DIR, safe_label, provider->name);
    if (written < 0 || (size_t)written >= sizeof(path)) return BRUCE_ERR_RESOURCE_LIMIT;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) return result;
    size_t written_size = 0;
    result = storage__write(file, location, strlen(location), &written_size);
    (void)storage__close(file);
    return result;
}

/* Deletes every /Network entry that belongs to a known provider (matched by
 * its "<name> " prefix) before repopulating, so a host removed from e.g.
 * "/.ssh/config" doesn't linger as a stale file forever. Anything whose name
 * doesn't start with a configured provider's name (a user's own file
 * dropped in there) is left alone. */
static void filemanager_network__clear_stale(
    const filemanager_network__provider_t providers[], size_t provider_count
) {
    /* Heap-allocated: 32 * sizeof(bruce_storage_entry_t) is over 3KB, too much
     * to add to the stack this runs on -- see filemanager_network__refresh(),
     * which calls this before ever entering filemanager_app_main()'s own
     * dialog loop, so this frame stacks directly on top of whatever's already
     * live in the caller. */
    bruce_storage_entry_t *entries = memory__malloc(32 * sizeof(bruce_storage_entry_t));
    if (entries == NULL) return;
    size_t count = 0;
    if (storage__list(FILEMANAGER_NETWORK_DIR, entries, 32, &count) != BRUCE_OK) {
        memory__free(entries);
        return;
    }
    if (count > 32) count = 32;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].type != BRUCE_STORAGE_ENTRY_FILE) continue;
        char owner[FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];
        if (!filemanager_network__split_entry_name(entries[i].name, owner, sizeof(owner))) continue;
        if (filemanager_network__find_provider(providers, provider_count, owner) == NULL) continue;

        /* entries[i].name is a char[BRUCE_STORAGE_NAME_MAX] field, but GCC's
         * -Werror=format-truncation doesn't infer that array's declared size just
         * from the expression here, so it has to be spelled out via precision for
         * the checker to see this can't overflow `path`. */
        char path[BRUCE_STORAGE_PATH_MAX];
        snprintf(
            path, sizeof(path), "%s/%.*s", FILEMANAGER_NETWORK_DIR, BRUCE_STORAGE_NAME_MAX - 1, entries[i].name
        );
        (void)storage__remove(path);
    }
    memory__free(entries);
}

void filemanager_network__refresh(void) {
    bool exists = false;
    if (storage__exists(FILEMANAGER_NETWORK_DIR, &exists) != BRUCE_OK) return;
    if (!exists && storage__mkdir(FILEMANAGER_NETWORK_DIR) != BRUCE_OK) return;

    /* Heap-allocated, not a stack array: this runs unconditionally at the top
     * of filemanager_app_main(), before any of its own locals go out of
     * scope, so this and every buffer below it stack directly on top of that
     * frame -- see the identical reasoning on filemanager_network__resolve_program()
     * and filemanager_network__clear_stale()'s entries[]. */
    filemanager_network__provider_t *providers =
        memory__malloc(FILEMANAGER_NETWORK_PROVIDER_MAX * sizeof(filemanager_network__provider_t));
    if (providers == NULL) return;
    size_t provider_count = 0;
    filemanager_network__load_providers(providers, FILEMANAGER_NETWORK_PROVIDER_MAX, &provider_count);
    if (provider_count == 0) {
        memory__free(providers);
        return;
    }

    filemanager_network__clear_stale(providers, provider_count);

    char *capture = memory__malloc(FILEMANAGER_NETWORK_CAPTURE_MAX);
    if (capture == NULL) {
        memory__free(providers);
        return;
    }
    for (size_t p = 0; p < provider_count; ++p) {
        size_t capture_size = 0;
        if (filemanager_network__capture(&providers[p], capture, FILEMANAGER_NETWORK_CAPTURE_MAX, &capture_size) !=
            BRUCE_OK) {
            continue;
        }
        char *saveptr = NULL;
        char *line = strtok_r(capture, "\n", &saveptr);
        while (line != NULL) {
            size_t len = strlen(line);
            while (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
            char *tab = strchr(line, '\t');
            if (tab != NULL) {
                *tab = '\0';
                const char *display_name = line;
                const char *location = tab + 1;
                if (display_name[0] != '\0') {
                    (void)filemanager_network__write_location(&providers[p], display_name, location);
                }
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    memory__free(capture);
    memory__free(providers);
}

/** @} */
