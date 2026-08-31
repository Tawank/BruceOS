#include "core_sdk/filetype.h"

#include "core/storage/storage.h"

#include "core_sdk/icon.h"
#include "core_sdk/storage.h"

#include "embedded_resources.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

/*
 * Owns "/config/extensions.conf" (schema: {"types": [{description,
 * program, extensions, mimetypes, interpreters, icon, actions}, ...]}) and
 * the magic-byte/shebang/text-heuristic fallbacks used when a path's
 * extension either isn't in that table or doesn't exist at all. See
 * core_sdk/filetype.h for the full detection order.
 *
 * Each entry's "extensions" only ever groups extensions that share one
 * exact MIME type (e.g. .jpg/.jpeg -> image/jpeg, .c/.h -> text/x-c), so
 * "mimetypes" -- read as mimetypes[0] via filetype__first_of_array_field()
 * -- is unambiguous per entry despite being a JSON array rather than one
 * string; the array shape is just what lets a future entry list more than
 * one equally-valid MIME string for the same kind (e.g. an alias) without a
 * schema change.
 *
 * "extensions" entries are also matched compound-first: a path ending in
 * two dot-segments (e.g. "backup.tar.gz") is looked up by both its full
 * ".tar.gz" and its bare ".gz" before falling back to whichever one has a
 * configured entry, so ".tar.gz" can have its own program/icon/actions
 * distinct from every other ".gz" file's (see filetype__entry_for_extension()).
 *
 * "actions" (optional): [{"label", "program"}, ...], up to
 * BRUCE_FILETYPE_MAX_ACTIONS entries -- extra file-manager context-menu
 * items beyond the default set, e.g. an archive's [{"label": "Extract
 * here", "program": "archive-extract"}]. `program` is run with the file's
 * path as its sole argument when chosen.
 */

#define FILETYPE_HEADER_SAMPLE_SIZE 256

static const char *FILETYPE_DEFAULT_CONFIG_JSON = json_extensions_json;

static cJSON *s_config;
static bool s_config_loaded;

static void filetype__load_config(void) {
    if (s_config_loaded) return;
    s_config_loaded = true;

    char *text = NULL;
    size_t size = 0;
    if (storage__read_file("/config/extensions.conf", &text, &size) && text != NULL) {
        s_config = cJSON_ParseWithLength(text, size);
        storage__free(text);
        if (s_config != NULL && cJSON_IsObject(s_config)) return;
        cJSON_Delete(s_config);
        s_config = NULL;
    }

    s_config = cJSON_Parse(FILETYPE_DEFAULT_CONFIG_JSON);
    if (s_config != NULL) {
        (void)storage__mkdir_internal("/config");
        (void)storage__write_file_atomic(
            "/config/extensions.conf", FILETYPE_DEFAULT_CONFIG_JSON, strlen(FILETYPE_DEFAULT_CONFIG_JSON)
        );
    }
}

static bool filetype__string_field(const cJSON *entry, const char *name, char *out, size_t out_size) {
    cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsString(field) || field->valuestring == NULL || field->valuestring[0] == '\0') return false;
    if (strlen(field->valuestring) >= out_size) return false;
    strncpy(out, field->valuestring, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

/* First element of a string array field, e.g. "mimetypes": [...] -> its [0]. */
static bool filetype__first_of_array_field(const cJSON *entry, const char *name, char *out, size_t out_size) {
    cJSON *array = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsArray(array)) return false;
    cJSON *first = cJSON_GetArrayItem(array, 0);
    if (!cJSON_IsString(first) || first->valuestring == NULL || strlen(first->valuestring) >= out_size) return false;
    strncpy(out, first->valuestring, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

static bool filetype__string_array_has_ci(const cJSON *entry, const char *field_name, const char *value) {
    cJSON *array = cJSON_GetObjectItemCaseSensitive(entry, field_name);
    if (!cJSON_IsArray(array)) return false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsString(item) && item->valuestring != NULL && strcasecmp(item->valuestring, value) == 0) {
            return true;
        }
    }
    return false;
}

static const char *filetype__extension_of(const char *path) {
    const char *dot = strrchr(path, '.');
    /* A dot with nothing after it, or one that's only part of a leading
     * dotfile name (e.g. ".bashrc"), isn't an extension. */
    if (dot == NULL || dot[1] == '\0') return NULL;
    const char *slash = strrchr(path, '/');
    if (dot == path || (slash != NULL && dot == slash + 1)) return NULL;
    return dot;
}

/* The two-segment compound extension ending at `simple` (filetype__extension_of()'s
 * result), e.g. "backup.tar.gz" -> ".tar.gz" for simple=".gz", or NULL if
 * there's no second dot within the same basename (or `simple` is NULL). */
static const char *filetype__compound_extension_of(const char *path, const char *simple) {
    if (simple == NULL) return NULL;
    const char *slash = strrchr(path, '/');
    const char *basename_start = slash != NULL ? slash + 1 : path;
    for (const char *p = simple; p > basename_start; --p) {
        if (p[-1] == '.') return p - 1;
    }
    return NULL;
}

/* Entry in the config's "types" array whose "extensions" list contains
 * path's extension, or NULL if path has no extension or nothing matches.
 * Tries the compound two-segment extension (".tar.gz") before the bare
 * final one (".gz") when both exist, so a configured ".tar.gz" entry wins
 * over the ".gz" entry every gzip file also matches. */
static cJSON *filetype__entry_for_extension(const char *path) {
    filetype__load_config();
    if (s_config == NULL || path == NULL) return NULL;
    const char *simple = filetype__extension_of(path);
    if (simple == NULL) return NULL;
    const char *compound = filetype__compound_extension_of(path, simple);
    cJSON *types = cJSON_GetObjectItemCaseSensitive(s_config, "types");
    if (!cJSON_IsArray(types)) return NULL;

    if (compound != NULL) {
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, types) {
            if (cJSON_IsObject(entry) && filetype__string_array_has_ci(entry, "extensions", compound)) return entry;
        }
    }
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, types) {
        if (cJSON_IsObject(entry) && filetype__string_array_has_ci(entry, "extensions", simple)) return entry;
    }
    return NULL;
}

/* Entry whose "interpreters" list contains `interpreter` (case-sensitive,
 * matching how shebangs are conventionally written). */
static cJSON *filetype__entry_for_interpreter(const char *interpreter) {
    filetype__load_config();
    if (s_config == NULL || interpreter == NULL || interpreter[0] == '\0') return NULL;
    cJSON *types = cJSON_GetObjectItemCaseSensitive(s_config, "types");
    if (!cJSON_IsArray(types)) return NULL;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, types) {
        if (!cJSON_IsObject(entry)) continue;
        cJSON *interpreters = cJSON_GetObjectItemCaseSensitive(entry, "interpreters");
        if (!cJSON_IsArray(interpreters)) continue;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, interpreters) {
            if (cJSON_IsString(item) && item->valuestring != NULL && strcmp(item->valuestring, interpreter) == 0) {
                return entry;
            }
        }
    }
    return NULL;
}

/* Fills out_info->actions[]/action_count from entry's optional "actions"
 * array; entries missing a non-empty "label" or "program" are skipped, and
 * anything past BRUCE_FILETYPE_MAX_ACTIONS is silently dropped. */
static void filetype__fill_actions(const cJSON *entry, bruce_filetype_info_t *out_info) {
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(entry, "actions");
    if (!cJSON_IsArray(actions)) return;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, actions) {
        if (out_info->action_count >= BRUCE_FILETYPE_MAX_ACTIONS) break;
        if (!cJSON_IsObject(item)) continue;
        bruce_filetype_action_t *action = &out_info->actions[out_info->action_count];
        if (!filetype__string_field(item, "label", action->label, sizeof(action->label))) continue;
        if (!filetype__string_field(item, "program", action->program, sizeof(action->program))) continue;
        out_info->action_count++;
    }
}

static void filetype__fill_from_entry(const cJSON *entry, bruce_filetype_info_t *out_info) {
    if (!filetype__string_field(entry, "description", out_info->description, sizeof(out_info->description))) {
        snprintf(out_info->description, sizeof(out_info->description), "data");
    }
    filetype__first_of_array_field(entry, "mimetypes", out_info->mimetype, sizeof(out_info->mimetype));
    filetype__string_field(entry, "program", out_info->program, sizeof(out_info->program));
    if (!filetype__string_field(entry, "icon", out_info->icon, sizeof(out_info->icon)) ||
        icon__get(out_info->icon) == NULL) {
        snprintf(out_info->icon, sizeof(out_info->icon), "file");
    }
    out_info->action_count = 0;
    filetype__fill_actions(entry, out_info);
}

bruce_result_t filetype__lookup_extension(const char *path, bruce_filetype_info_t *out_info) {
    if (path == NULL || out_info == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    snprintf(out_info->icon, sizeof(out_info->icon), "file");
    cJSON *entry = filetype__entry_for_extension(path);
    if (entry != NULL) filetype__fill_from_entry(entry, out_info);
    return BRUCE_OK;
}

const char *filetype__icon_for_path(const char *path) {
    cJSON *entry = path != NULL ? filetype__entry_for_extension(path) : NULL;
    cJSON *icon = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "icon") : NULL;
    if (cJSON_IsString(icon) && icon->valuestring != NULL && icon->valuestring[0] != '\0' &&
        icon__get(icon->valuestring) != NULL) {
        return icon->valuestring;
    }
    return "file";
}

/* ---- Magic-byte signatures ---- */

typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t offset; /* where in the file the signature starts */
    const char *description;
    const char *mimetype;
    const char *icon;
} filetype_signature_t;

static const uint8_t FILETYPE_SIG_PNG[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
static const uint8_t FILETYPE_SIG_JPEG[] = {0xff, 0xd8, 0xff};
static const uint8_t FILETYPE_SIG_GIF87[] = {'G', 'I', 'F', '8', '7', 'a'};
static const uint8_t FILETYPE_SIG_GIF89[] = {'G', 'I', 'F', '8', '9', 'a'};
static const uint8_t FILETYPE_SIG_BMP[] = {'B', 'M'};
static const uint8_t FILETYPE_SIG_ZIP[] = {'P', 'K', 0x03, 0x04};
static const uint8_t FILETYPE_SIG_ZIP_EMPTY[] = {'P', 'K', 0x05, 0x06};
static const uint8_t FILETYPE_SIG_GZIP[] = {0x1f, 0x8b};
static const uint8_t FILETYPE_SIG_ELF[] = {0x7f, 'E', 'L', 'F'};
static const uint8_t FILETYPE_SIG_WASM[] = {0x00, 'a', 's', 'm'};
static const uint8_t FILETYPE_SIG_RIFF[] = {'R', 'I', 'F', 'F'};
static const uint8_t FILETYPE_SIG_FLAC[] = {'f', 'L', 'a', 'C'};
static const uint8_t FILETYPE_SIG_OGG[] = {'O', 'g', 'g', 'S'};
static const uint8_t FILETYPE_SIG_7Z[] = {'7', 'z', 0xbc, 0xaf, 0x27, 0x1c};
static const uint8_t FILETYPE_SIG_RAR[] = {'R', 'a', 'r', '!', 0x1a, 0x07};

/* Longest/most specific signatures first, so e.g. RIFF's generic 4 bytes
 * never shadows something that also starts with them. */
static const filetype_signature_t FILETYPE_SIGNATURES[] = {
    {FILETYPE_SIG_PNG, sizeof(FILETYPE_SIG_PNG), 0, "PNG image", "image/png", "file-image"},
    {FILETYPE_SIG_GIF87, sizeof(FILETYPE_SIG_GIF87), 0, "GIF image", "image/gif", "file-image"},
    {FILETYPE_SIG_GIF89, sizeof(FILETYPE_SIG_GIF89), 0, "GIF image", "image/gif", "file-image"},
    {FILETYPE_SIG_JPEG, sizeof(FILETYPE_SIG_JPEG), 0, "JPEG image", "image/jpeg", "file-image"},
    {FILETYPE_SIG_BMP, sizeof(FILETYPE_SIG_BMP), 0, "BMP image", "image/bmp", "file-image"},
    {FILETYPE_SIG_7Z, sizeof(FILETYPE_SIG_7Z), 0, "7-Zip archive", "application/x-7z-compressed", "zip-box"},
    {FILETYPE_SIG_RAR, sizeof(FILETYPE_SIG_RAR), 0, "RAR archive", "application/vnd.rar", "zip-box"},
    {FILETYPE_SIG_ZIP, sizeof(FILETYPE_SIG_ZIP), 0, "ZIP archive", "application/zip", "zip-box"},
    {FILETYPE_SIG_ZIP_EMPTY, sizeof(FILETYPE_SIG_ZIP_EMPTY), 0, "ZIP archive", "application/zip", "zip-box"},
    {FILETYPE_SIG_GZIP, sizeof(FILETYPE_SIG_GZIP), 0, "Gzip-compressed data", "application/gzip", "zip-box"},
    {FILETYPE_SIG_ELF, sizeof(FILETYPE_SIG_ELF), 0, "ELF executable", "application/x-elf", "application"},
    {FILETYPE_SIG_WASM, sizeof(FILETYPE_SIG_WASM), 0, "WebAssembly application", "application/wasm", "application"},
    {FILETYPE_SIG_FLAC, sizeof(FILETYPE_SIG_FLAC), 0, "FLAC audio", "audio/flac", "file-music"},
    {FILETYPE_SIG_OGG, sizeof(FILETYPE_SIG_OGG), 0, "Ogg audio", "audio/ogg", "file-music"},
    {FILETYPE_SIG_RIFF, sizeof(FILETYPE_SIG_RIFF), 0, "RIFF media (AVI/WAV)", "application/octet-stream",
     "file-video"},
};
#define FILETYPE_SIGNATURE_COUNT (sizeof(FILETYPE_SIGNATURES) / sizeof(FILETYPE_SIGNATURES[0]))

static const filetype_signature_t *filetype__match_signature(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < FILETYPE_SIGNATURE_COUNT; ++i) {
        const filetype_signature_t *signature = &FILETYPE_SIGNATURES[i];
        if (size < signature->offset + signature->length) continue;
        if (memcmp(bytes + signature->offset, signature->bytes, signature->length) == 0) return signature;
    }
    return NULL;
}

/* ---- Shebang ---- */

/* Extracts the interpreter's basename from a "#!/path/to/prog [args]" (or
 * "#!/usr/bin/env prog [args]") first line into `out`. Returns false if
 * `bytes` doesn't start with a shebang or the interpreter name won't fit. */
static bool filetype__parse_shebang(const uint8_t *bytes, size_t size, char *out, size_t out_size) {
    if (size < 3 || bytes[0] != '#' || bytes[1] != '!') return false;
    size_t i = 2;
    while (i < size && (bytes[i] == ' ' || bytes[i] == '\t')) i++;
    size_t line_end = i;
    while (line_end < size && bytes[line_end] != '\n' && bytes[line_end] != '\r') line_end++;
    if (line_end == i) return false;

    /* Take the last '/'-separated component of the first whitespace-delimited
     * token; if that token is "env", the interpreter is the next token
     * instead (the "#!/usr/bin/env python3" convention). */
    size_t token_start = i;
    size_t token_end = i;
    while (token_end < line_end && bytes[token_end] != ' ' && bytes[token_end] != '\t') token_end++;
    size_t name_offset = token_start;
    for (size_t j = token_start; j < token_end; ++j) {
        if (bytes[j] == '/') name_offset = j + 1;
    }
    const char *name_start = (const char *)bytes + name_offset;
    size_t name_length = token_end - name_offset;

    if (name_length == 3 && strncmp(name_start, "env", 3) == 0) {
        size_t next_start = token_end;
        while (next_start < line_end && (bytes[next_start] == ' ' || bytes[next_start] == '\t')) next_start++;
        size_t next_end = next_start;
        while (next_end < line_end && bytes[next_end] != ' ' && bytes[next_end] != '\t') next_end++;
        if (next_end == next_start) return false;
        name_start = (const char *)bytes + next_start;
        name_length = next_end - next_start;
    }

    if (name_length == 0 || name_length >= out_size) return false;
    memcpy(out, name_start, name_length);
    out[name_length] = '\0';
    return true;
}

/* ---- Text/binary heuristic ---- */

static bool filetype__looks_binary(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = bytes[i];
        if (byte == '\0') return true;
        if (byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t') return true;
    }
    return false;
}

static void filetype__identify_from_bytes_locked(
    const char *path, const uint8_t *bytes, size_t size, bruce_filetype_info_t *out_info
) {
    memset(out_info, 0, sizeof(*out_info));

    cJSON *extension_entry = filetype__entry_for_extension(path);
    if (extension_entry != NULL) {
        filetype__fill_from_entry(extension_entry, out_info);
        out_info->is_binary = filetype__looks_binary(bytes, size);
        return;
    }

    const filetype_signature_t *signature = filetype__match_signature(bytes, size);
    if (signature != NULL) {
        snprintf(out_info->description, sizeof(out_info->description), "%s", signature->description);
        snprintf(out_info->mimetype, sizeof(out_info->mimetype), "%s", signature->mimetype);
        snprintf(out_info->icon, sizeof(out_info->icon), "%s", signature->icon);
        out_info->is_binary = true;
        return;
    }

    char interpreter[BRUCE_FILETYPE_PROGRAM_MAX];
    if (filetype__parse_shebang(bytes, size, interpreter, sizeof(interpreter))) {
        cJSON *interpreter_entry = filetype__entry_for_interpreter(interpreter);
        if (interpreter_entry != NULL) {
            filetype__fill_from_entry(interpreter_entry, out_info);
        } else {
            snprintf(out_info->description, sizeof(out_info->description), "%s script", interpreter);
            snprintf(out_info->mimetype, sizeof(out_info->mimetype), "text/plain");
            snprintf(out_info->icon, sizeof(out_info->icon), "file-code");
        }
        out_info->is_binary = false;
        return;
    }

    out_info->is_binary = filetype__looks_binary(bytes, size);
    if (out_info->is_binary) {
        snprintf(out_info->description, sizeof(out_info->description), "data");
        snprintf(out_info->icon, sizeof(out_info->icon), "file");
    } else {
        snprintf(out_info->description, sizeof(out_info->description), "ASCII text");
        snprintf(out_info->mimetype, sizeof(out_info->mimetype), "text/plain");
        snprintf(out_info->icon, sizeof(out_info->icon), "file-document");
    }
}

bruce_result_t filetype__identify_bytes(
    const char *path, const uint8_t *bytes, size_t size, bruce_filetype_info_t *out_info
) {
    if (out_info == NULL || (bytes == NULL && size > 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    filetype__identify_from_bytes_locked(path != NULL && path[0] != '\0' ? path : NULL, bytes, size, out_info);
    return BRUCE_OK;
}

bruce_result_t filetype__identify(const char *path, bruce_filetype_info_t *out_info) {
    if (path == NULL || path[0] == '\0' || out_info == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    /* storage__list() only ever succeeds on a directory (same probe used by
     * filemanager_app.c and shell_builtins.c), so it's the cheapest way to
     * tell directories from files up front. */
    size_t entry_count = 0;
    if (storage__list(path, NULL, 0, &entry_count) == BRUCE_OK) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->is_directory = true;
        snprintf(out_info->icon, sizeof(out_info->icon), "folder");
        snprintf(out_info->description, sizeof(out_info->description), "directory");
        return BRUCE_OK;
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint8_t sample[FILETYPE_HEADER_SAMPLE_SIZE];
    size_t sample_size = 0;
    result = storage__read(file, sample, sizeof(sample), &sample_size);
    (void)storage__close(file);
    if (result != BRUCE_OK) return result;

    filetype__identify_from_bytes_locked(path, sample, sample_size, out_info);
    return BRUCE_OK;
}
