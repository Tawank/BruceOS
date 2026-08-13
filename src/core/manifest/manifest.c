#include "core_sdk/manifest.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/storage.h"

/* ----------------------------------------------------------------------- */
/* JSON manifest parser (shared)                                            */
/* ----------------------------------------------------------------------- */

#define MANIFEST__STACK_MIN 1024u
#define MANIFEST__STACK_MAX 2097152u
#define MANIFEST__STACK_DEFAULT 4096u

#define MANIFEST_JS_MAX_BYTES 2048u
#define MANIFEST_JS_GENERIC_ICON_BYTE 0xAAu

static const uint8_t s_generic_icon[BRUCE_MANIFEST_ICON_BYTES] = {
    MANIFEST_JS_GENERIC_ICON_BYTE,
    MANIFEST_JS_GENERIC_ICON_BYTE,
    MANIFEST_JS_GENERIC_ICON_BYTE,
    MANIFEST_JS_GENERIC_ICON_BYTE,
    MANIFEST_JS_GENERIC_ICON_BYTE,
    MANIFEST_JS_GENERIC_ICON_BYTE,
    MANIFEST_JS_GENERIC_ICON_BYTE,
    MANIFEST_JS_GENERIC_ICON_BYTE,
};

static int manifest__base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool manifest__base64_decode_exact(const char *in, uint8_t *out, size_t out_size) {
    size_t in_len = strlen(in);
    if (in_len == 0 || in_len % 4 != 0) { return false; }

    size_t pad = 0;
    if (in[in_len - 1] == '=') pad++;
    if (in_len >= 2 && in[in_len - 2] == '=') pad++;

    size_t decoded_len = (in_len / 4) * 3 - pad;
    if (decoded_len != out_size) { return false; }

    size_t out_index = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        char c2 = in[i + 2];
        char c3 = in[i + 3];
        int v0 = manifest__base64_value(in[i]);
        int v1 = manifest__base64_value(in[i + 1]);
        int v2 = (c2 == '=') ? 0 : manifest__base64_value(c2);
        int v3 = (c3 == '=') ? 0 : manifest__base64_value(c3);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) { return false; }

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        if (out_index < out_size) out[out_index++] = (uint8_t)(triple >> 16);
        if (c2 != '=' && out_index < out_size) out[out_index++] = (uint8_t)(triple >> 8);
        if (c3 != '=' && out_index < out_size) out[out_index++] = (uint8_t)triple;
    }
    return out_index == out_size;
}

bruce_manifest_t *manifest__parse(const char *json, size_t json_len) {
    if (json == NULL || json_len == 0) { return NULL; }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }

    bruce_manifest_t *out_manifest = memory__malloc(sizeof(bruce_manifest_t));
    if (out_manifest == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    memset(out_manifest, 0, sizeof(*out_manifest));

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "appName");
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(root, "appIcon");
    const cJSON *abi = cJSON_GetObjectItemCaseSensitive(root, "coreAbiVersion");
    const cJSON *stack = cJSON_GetObjectItemCaseSensitive(root, "stackSize");
    const cJSON *permissions = cJSON_GetObjectItemCaseSensitive(root, "permissions");

    bool ok = cJSON_IsString(name) && name->valuestring != NULL && name->valuestring[0] != '\0' &&
              strlen(name->valuestring) < BRUCE_MANIFEST_APP_NAME_MAX;
    ok = ok && cJSON_IsString(icon) && icon->valuestring != NULL;
    ok = ok && cJSON_IsNumber(abi);
    ok = ok && cJSON_IsNumber(stack) && stack->valuedouble >= MANIFEST__STACK_MIN &&
         stack->valuedouble <= MANIFEST__STACK_MAX;
    ok = ok && (permissions == NULL || cJSON_IsArray(permissions));
    if (!ok) {
        memory__free(out_manifest);
        cJSON_Delete(root);
        return NULL;
    }

    if (!manifest__base64_decode_exact(
            icon->valuestring, out_manifest->app_icon, BRUCE_MANIFEST_ICON_BYTES
        )) {
        memory__free(out_manifest);
        cJSON_Delete(root);
        return NULL;
    }

    strncpy(out_manifest->app_name, name->valuestring, BRUCE_MANIFEST_APP_NAME_MAX - 1);
    out_manifest->core_abi_version = (uint32_t)abi->valuedouble;
    out_manifest->stack_size = (uint32_t)stack->valuedouble;

    size_t permission_count = 0;
    if (permissions != NULL) {
        int array_size = cJSON_GetArraySize(permissions);
        if (array_size < 0 || (size_t)array_size > BRUCE_MANIFEST_MAX_PERMISSIONS) {
            memory__free(out_manifest);
            cJSON_Delete(root);
            return NULL;
        }
        for (int i = 0; i < array_size; ++i) {
            const cJSON *entry = cJSON_GetArrayItem(permissions, i);
            bruce_permission_t permission;
            if (!cJSON_IsString(entry) || entry->valuestring == NULL ||
                !permission__from_name(entry->valuestring, &permission)) {
                memory__free(out_manifest);
                cJSON_Delete(root);
                return NULL;
            }
            bool duplicate = false;
            for (size_t j = 0; j < permission_count; ++j) {
                if (strcmp(out_manifest->permissions[j], entry->valuestring) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate || strlen(entry->valuestring) >= BRUCE_MANIFEST_PERMISSION_NAME_MAX) {
                memory__free(out_manifest);
                cJSON_Delete(root);
                return NULL;
            }
            strncpy(
                out_manifest->permissions[permission_count],
                entry->valuestring,
                BRUCE_MANIFEST_PERMISSION_NAME_MAX - 1
            );
            permission_count++;
        }
    }
    out_manifest->permission_count = permission_count;

    cJSON_Delete(root);
    return out_manifest;
}

/* ----------------------------------------------------------------------- */
/* File-I/O helpers                                                          */
/* ----------------------------------------------------------------------- */

static bool manifest__pread(bruce_file_id_t file, uint64_t offset, void *buffer, size_t size) {
    if (storage__seek(file, (int64_t)offset, SEEK_SET, NULL) != BRUCE_OK) { return false; }
    uint8_t *out = (uint8_t *)buffer;
    size_t total = 0;
    while (total < size) {
        size_t chunk = 0;
        if (storage__read(file, out + total, size - total, &chunk) != BRUCE_OK || chunk == 0) {
            return false;
        }
        total += chunk;
    }
    return true;
}

/* ----------------------------------------------------------------------- */
/* ELF-specific helpers                                                      */
/* ----------------------------------------------------------------------- */

#if CONFIG_IDF_TARGET_ARCH_RISCV
#define MANIFEST_ELF_EXPECTED_MACHINE 243u /* EM_RISCV */
#else
#define MANIFEST_ELF_EXPECTED_MACHINE 94u /* EM_XTENSA */
#endif

#define MANIFEST_ELF_SECTION_NAME ".bruce.manifest"
#define MANIFEST_ELF_SHF_ALLOC 0x2u
#define MANIFEST_ELF_MAX_MANIFEST_BYTES 2048u
#define MANIFEST_WASM_SECTION_NAME "bruce.manifest"
#define MANIFEST_WASM_MAX_MANIFEST_BYTES 2048u

typedef struct __attribute__((packed)) {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} manifest_elf_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} manifest_elf_shdr_t;

typedef enum {
    MANIFEST_ELF_READ_NONE,
    MANIFEST_ELF_READ_HEADER,
    MANIFEST_ELF_READ_SECTION_TABLE,
    MANIFEST_ELF_READ_SECTION_NOT_FOUND,
    MANIFEST_ELF_READ_SECTION_METADATA,
    MANIFEST_ELF_READ_SECTION_PAYLOAD,
    MANIFEST_ELF_READ_JSON,
} manifest_elf_read_stage_t;

static const char *manifest__elf_read_failure(manifest_elf_read_stage_t stage) {
    switch (stage) {
        case MANIFEST_ELF_READ_HEADER: return "ELF header or section indexes are invalid";
        case MANIFEST_ELF_READ_SECTION_TABLE: return "ELF section table is unreadable or truncated";
        case MANIFEST_ELF_READ_SECTION_NOT_FOUND: return ".bruce.manifest section was not found";
        case MANIFEST_ELF_READ_SECTION_METADATA: return ".bruce.manifest flags or size are invalid";
        case MANIFEST_ELF_READ_SECTION_PAYLOAD: return ".bruce.manifest payload is unreadable or truncated";
        case MANIFEST_ELF_READ_JSON: return ".bruce.manifest JSON is invalid";
        default: return "invalid ELF manifest";
    }
}

/* Reads the .bruce.manifest section raw bytes from an already-open ELF
 * file.  Does NOT validate e_machine or ELF magic — the caller is
 * responsible for that. Returns process-owned bytes in *out_bytes. */
static bruce_result_t manifest__read_elf_manifest_bytes(
    bruce_file_id_t file, char **out_bytes, size_t *out_len, manifest_elf_read_stage_t *out_stage
) {
    if (out_stage != NULL) *out_stage = MANIFEST_ELF_READ_HEADER;
    manifest_elf_ehdr_t header;
    if (!manifest__pread(file, 0, &header, sizeof(header))) { return BRUCE_ERR_MANIFEST_INVALID; }
    if (header.e_shnum == 0 || header.e_shstrndx >= header.e_shnum) { return BRUCE_ERR_MANIFEST_INVALID; }

    if (out_stage != NULL) *out_stage = MANIFEST_ELF_READ_SECTION_TABLE;
    manifest_elf_shdr_t shstrtab_hdr;
    if (!manifest__pread(
            file,
            header.e_shoff + (uint64_t)header.e_shstrndx * header.e_shentsize,
            &shstrtab_hdr,
            sizeof(shstrtab_hdr)
        )) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    for (uint16_t i = 0; i < header.e_shnum; ++i) {
        manifest_elf_shdr_t section;
        if (!manifest__pread(
                file, header.e_shoff + (uint64_t)i * header.e_shentsize, &section, sizeof(section)
            )) {
            return BRUCE_ERR_MANIFEST_INVALID;
        }

        char name[sizeof(MANIFEST_ELF_SECTION_NAME)];
        if (!manifest__pread(file, shstrtab_hdr.sh_offset + section.sh_name, name, sizeof(name))) {
            continue;
        }
        name[sizeof(name) - 1] = '\0';
        if (strcmp(name, MANIFEST_ELF_SECTION_NAME) != 0) { continue; }
        if ((section.sh_flags & MANIFEST_ELF_SHF_ALLOC) != 0 || section.sh_size == 0 ||
            section.sh_size > MANIFEST_ELF_MAX_MANIFEST_BYTES) {
            if (out_stage != NULL) *out_stage = MANIFEST_ELF_READ_SECTION_METADATA;
            return BRUCE_ERR_MANIFEST_INVALID;
        }

        char *bytes = memory__malloc(section.sh_size + 1);
        if (bytes == NULL) { return BRUCE_ERR_NO_MEMORY; }
        if (out_stage != NULL) *out_stage = MANIFEST_ELF_READ_SECTION_PAYLOAD;
        if (!manifest__pread(file, section.sh_offset, bytes, section.sh_size)) {
            memory__free(bytes);
            return BRUCE_ERR_MANIFEST_INVALID;
        }
        bytes[section.sh_size] = '\0';
        *out_bytes = bytes;
        *out_len = section.sh_size;
        if (out_stage != NULL) *out_stage = MANIFEST_ELF_READ_NONE;
        return BRUCE_OK;
    }

    if (out_stage != NULL) *out_stage = MANIFEST_ELF_READ_SECTION_NOT_FOUND;
    return BRUCE_ERR_MANIFEST_INVALID;
}

/* ----------------------------------------------------------------------- */
/* Path normalization (accepts absolute or "./" relative, maps to root)      */
/* ----------------------------------------------------------------------- */

static bool manifest__normalize_path(const char *path, char *out, size_t out_size) {
    if (path == NULL || strstr(path, "..") != NULL || out_size == 0) { return false; }
    int len;
    if (path[0] == '/') {
        len = snprintf(out, out_size, "%s", path);
    } else if (strncmp(path, "./", 2) == 0) {
        len = snprintf(out, out_size, "/%s", path + 2);
    } else {
        return false;
    }
    return len > 0 && (size_t)len < out_size;
}

/* ----------------------------------------------------------------------- */
/* Universal manifest JSON extractor                                        */
/* ----------------------------------------------------------------------- */

static bruce_result_t
manifest__read_js_manifest_bytes(bruce_file_id_t file, char **out_bytes, size_t *out_len);
static bruce_result_t manifest__read_wasm_manifest_bytes(
    bruce_file_id_t file, char **out_bytes, size_t *out_len, bool *out_module_valid
);

char *manifest__inspect_path(const char *path) {
    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!manifest__normalize_path(path, normalized_path, sizeof(normalized_path))) { return NULL; }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(normalized_path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) { return NULL; }

    char *out_json = NULL;
    bruce_result_t result;
    uint8_t magic[4];
    size_t path_len = strlen(normalized_path);
    bool is_js = path_len > 3 && strcmp(normalized_path + path_len - 3, ".js") == 0;
    bool magic_read = manifest__pread(file, 0, magic, sizeof(magic));
    if (magic_read && memcmp(
                          magic,
                          "\x7f"
                          "ELF",
                          4
                      ) == 0) {
        size_t out_len = 0;
        result = manifest__read_elf_manifest_bytes(file, &out_json, &out_len, NULL);
    } else if (magic_read && memcmp(magic, "\0asm", sizeof(magic)) == 0) {
        size_t out_len = 0;
        result = manifest__read_wasm_manifest_bytes(file, &out_json, &out_len, NULL);
    } else if (is_js) {
        size_t out_len = 0;
        result = manifest__read_js_manifest_bytes(file, &out_json, &out_len);
    } else {
        result = BRUCE_ERR_MANIFEST_INVALID;
    }

    storage__close(file);
    return result == BRUCE_OK ? out_json : NULL;
}

/* ----------------------------------------------------------------------- */
/* ELF-specific full inspection                                             */
/* ----------------------------------------------------------------------- */

bruce_app_inspection_t *manifest__inspect_elf(const char *path) {
    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!manifest__normalize_path(path, normalized_path, sizeof(normalized_path))) { return NULL; }

    bruce_app_inspection_t *out_inspection = memory__malloc(sizeof(*out_inspection));
    if (out_inspection == NULL) { return NULL; }
    memset(out_inspection, 0, sizeof(*out_inspection));

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(normalized_path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) {
        memory__free(out_inspection);
        return NULL;
    }

    manifest_elf_ehdr_t header;
    bruce_result_t result;
    bool header_valid = false;
    manifest_elf_read_stage_t read_stage = MANIFEST_ELF_READ_HEADER;
    if (!manifest__pread(file, 0, &header, sizeof(header))) {
        result = BRUCE_ERR_MANIFEST_INVALID;
    } else if (memcmp(
                   header.e_ident,
                   "\x7f"
                   "ELF",
                   4
               ) != 0 ||
               header.e_ident[4] != 1 /* ELFCLASS32 */) {
        result = BRUCE_ERR_MANIFEST_INVALID;
    } else if (header.e_machine != MANIFEST_ELF_EXPECTED_MACHINE) {
        result = BRUCE_ERR_TARGET_MISMATCH;
    } else {
        header_valid = true;
        char *bytes = NULL;
        size_t bytes_len = 0;
        result = manifest__read_elf_manifest_bytes(file, &bytes, &bytes_len, &read_stage);
        if (result == BRUCE_OK) {
            bruce_manifest_t *parsed = manifest__parse(bytes, bytes_len);
            memory__free(bytes);
            if (parsed != NULL) {
                out_inspection->kind = BRUCE_APP_KIND_ELF;
                out_inspection->manifest = *parsed;
                out_inspection->abi_warning =
                    out_inspection->manifest.core_abi_version != BRUCE_CORE_ABI_VERSION;
                memory__free(parsed);
                result = BRUCE_OK;
            } else {
                read_stage = MANIFEST_ELF_READ_JSON;
                result = BRUCE_ERR_MANIFEST_INVALID;
            }
        }
    }

    storage__close(file);
    if (result != BRUCE_OK) {
        const char *reason = result == BRUCE_ERR_TARGET_MISMATCH ? "ELF target does not match this device"
                             : result == BRUCE_ERR_NO_MEMORY     ? "not enough memory to inspect manifest"
                                                                 : manifest__elf_read_failure(read_stage);
        printf("manifest__inspect_elf: warning: %s: %s (%d)\n", normalized_path, reason, result);
        if (result == BRUCE_ERR_MANIFEST_INVALID && header_valid) {
            const char *base = strrchr(normalized_path, '/');
            base = base != NULL ? base + 1 : normalized_path;
            const char *extension = strrchr(base, '.');
            size_t name_len =
                extension != NULL && extension != base ? (size_t)(extension - base) : strlen(base);
            if (name_len >= sizeof(out_inspection->manifest.app_name)) {
                name_len = sizeof(out_inspection->manifest.app_name) - 1;
            }
            memcpy(out_inspection->manifest.app_name, base, name_len);
            out_inspection->manifest.app_name[name_len] = '\0';
            out_inspection->kind = BRUCE_APP_KIND_ELF;
            out_inspection->manifest.core_abi_version = BRUCE_CORE_ABI_VERSION;
            out_inspection->manifest.stack_size = MANIFEST__STACK_DEFAULT;
            printf(
                "manifest__inspect_elf: warning: %s: launching with filename, %u-byte stack, and no "
                "predeclared permissions\n",
                normalized_path,
                (unsigned)MANIFEST__STACK_DEFAULT
            );
            return out_inspection;
        }
        memory__free(out_inspection);
        return NULL;
    }
    return out_inspection;
}

/* ----------------------------------------------------------------------- */
/* JavaScript helpers and full inspection                                  */
/* ----------------------------------------------------------------------- */

/* Reads up to MANIFEST_JS_MAX_BYTES from the beginning of `file` into a
 * process-owned, NUL-terminated buffer. Returns BRUCE_OK or an error code. */
static bruce_result_t manifest__read_js_head(bruce_file_id_t file, char **out_bytes, size_t *out_len) {
    char *bytes = memory__malloc(MANIFEST_JS_MAX_BYTES + 1);
    if (bytes == NULL) { return BRUCE_ERR_NO_MEMORY; }

    size_t total = 0;
    while (total < MANIFEST_JS_MAX_BYTES) {
        size_t chunk = 0;
        if (storage__read(file, bytes + total, MANIFEST_JS_MAX_BYTES - total, &chunk) != BRUCE_OK) {
            memory__free(bytes);
            return BRUCE_ERR_IO;
        }
        if (chunk == 0) { break; }
        total += chunk;
    }
    bytes[total] = '\0';
    *out_bytes = bytes;
    *out_len = total;
    return BRUCE_OK;
}

/* If the JS head starts with a block comment (slash-asterisk ...
 * asterisk-slash), extract the content between the markers.  Returns NULL
 * if no leading block comment is present or if it is unclosed.  The returned
 * pointer is into 'head' and is not separately allocated; *out_len receives the
 * content length. */
static const char *manifest__extract_js_block_comment(const char *head, size_t head_len, size_t *out_len) {
    if (head_len < 4 || head[0] != '/' || head[1] != '*') { return NULL; }
    const char *end = strstr(head + 2, "*/");
    if (end == NULL) { return NULL; }
    *out_len = (size_t)(end - (head + 2));
    return head + 2;
}

/* Tries to parse the content of a leading JS comment block as canonical
 * manifest JSON. Returns a process-owned manifest on success, NULL otherwise. */
static bruce_manifest_t *manifest__parse_js_manifest_comment(const char *comment, size_t comment_len) {
    /* Skip leading whitespace; the trailing block-comment terminator is already excluded. */
    while (comment_len > 0 && (*comment == ' ' || *comment == '\t' || *comment == '\n' || *comment == '\r')) {
        comment++;
        comment_len--;
    }
    while (comment_len > 0 && (comment[comment_len - 1] == ' ' || comment[comment_len - 1] == '\t' ||
                               comment[comment_len - 1] == '\n' || comment[comment_len - 1] == '\r' ||
                               comment[comment_len - 1] == '*')) {
        comment_len--;
    }
    if (comment_len == 0) { return NULL; }
    return manifest__parse(comment, comment_len);
}

/* Builds a fallback manifest for an unmanifested JS file. */
static bruce_manifest_t *manifest__default_js_manifest(const char *path) {
    bruce_manifest_t *manifest = memory__malloc(sizeof(*manifest));
    if (manifest == NULL) { return NULL; }
    memset(manifest, 0, sizeof(*manifest));

    const char *base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    size_t name_len = strlen(base);
    if (name_len >= BRUCE_MANIFEST_APP_NAME_MAX) { name_len = BRUCE_MANIFEST_APP_NAME_MAX - 1; }
    memcpy(manifest->app_name, base, name_len);
    manifest->app_name[name_len] = '\0';

    memcpy(manifest->app_icon, s_generic_icon, BRUCE_MANIFEST_ICON_BYTES);
    manifest->core_abi_version = BRUCE_CORE_ABI_VERSION;
    manifest->stack_size = 8192;
    manifest->permission_count = 0;
    return manifest;
}

static bruce_manifest_t *manifest__default_wasm_manifest(const char *path) {
    bruce_manifest_t *manifest = memory__malloc(sizeof(*manifest));
    if (manifest == NULL) { return NULL; }
    memset(manifest, 0, sizeof(*manifest));

    const char *base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t name_len = dot != NULL && dot != base ? (size_t)(dot - base) : strlen(base);
    if (name_len >= BRUCE_MANIFEST_APP_NAME_MAX) { name_len = BRUCE_MANIFEST_APP_NAME_MAX - 1; }
    memcpy(manifest->app_name, base, name_len);
    manifest->app_name[name_len] = '\0';

    memcpy(manifest->app_icon, s_generic_icon, BRUCE_MANIFEST_ICON_BYTES);
    manifest->core_abi_version = BRUCE_CORE_ABI_VERSION;
    manifest->stack_size = 8192;
    manifest->permission_count = 0;
    return manifest;
}

static bool
manifest__read_wasm_u32(bruce_file_id_t file, uint64_t limit, uint64_t *offset, uint32_t *out_value) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (*offset >= limit) { return false; }
        uint8_t byte;
        if (!manifest__pread(file, *offset, &byte, sizeof(byte))) { return false; }
        (*offset)++;
        if (i == 4 && (byte & 0xf0u) != 0) { return false; }
        value |= (uint32_t)(byte & 0x7fu) << (i * 7);
        if ((byte & 0x80u) == 0) {
            *out_value = value;
            return true;
        }
    }
    return false;
}

static bruce_result_t manifest__read_wasm_manifest_bytes(
    bruce_file_id_t file, char **out_bytes, size_t *out_len, bool *out_module_valid
) {
    if (out_bytes != NULL) *out_bytes = NULL;
    if (out_len != NULL) *out_len = 0;
    if (out_module_valid != NULL) *out_module_valid = false;

    uint64_t file_size = 0;
    if (storage__seek(file, 0, SEEK_END, &file_size) != BRUCE_OK || file_size < 8) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    static const uint8_t wasm_header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    uint8_t header[sizeof(wasm_header)];
    if (!manifest__pread(file, 0, header, sizeof(header)) ||
        memcmp(header, wasm_header, sizeof(header)) != 0) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    char *manifest_bytes = NULL;
    size_t manifest_len = 0;
    bool manifest_invalid = false;
    uint64_t offset = sizeof(header);
    while (offset < file_size) {
        uint8_t section_id;
        if (!manifest__pread(file, offset, &section_id, sizeof(section_id))) { goto malformed; }
        offset++;

        uint32_t section_size;
        if (!manifest__read_wasm_u32(file, file_size, &offset, &section_size) ||
            (uint64_t)section_size > file_size - offset) {
            goto malformed;
        }
        uint64_t section_end = offset + section_size;

        if (section_id == 0) {
            uint32_t name_len;
            if (!manifest__read_wasm_u32(file, section_end, &offset, &name_len) ||
                (uint64_t)name_len > section_end - offset) {
                goto malformed;
            }

            bool is_manifest = name_len == sizeof(MANIFEST_WASM_SECTION_NAME) - 1;
            char name[sizeof(MANIFEST_WASM_SECTION_NAME)];
            if (is_manifest) {
                if (!manifest__pread(file, offset, name, name_len)) { goto malformed; }
                name[name_len] = '\0';
                is_manifest = strcmp(name, MANIFEST_WASM_SECTION_NAME) == 0;
            }
            offset += name_len;

            if (is_manifest) {
                size_t payload_len = (size_t)(section_end - offset);
                if (manifest_bytes != NULL || payload_len == 0 ||
                    payload_len > MANIFEST_WASM_MAX_MANIFEST_BYTES) {
                    manifest_invalid = true;
                    memory__free(manifest_bytes);
                    manifest_bytes = NULL;
                    manifest_len = 0;
                } else if (!manifest_invalid) {
                    manifest_bytes = memory__malloc(payload_len + 1);
                    if (manifest_bytes == NULL) { return BRUCE_ERR_NO_MEMORY; }
                    if (!manifest__pread(file, offset, manifest_bytes, payload_len)) { goto malformed; }
                    manifest_bytes[payload_len] = '\0';
                    manifest_len = payload_len;
                }
            }
        }
        offset = section_end;
    }

    if (out_module_valid != NULL) *out_module_valid = true;
    if (manifest_invalid || manifest_bytes == NULL) {
        memory__free(manifest_bytes);
        return BRUCE_ERR_MANIFEST_INVALID;
    }
    *out_bytes = manifest_bytes;
    *out_len = manifest_len;
    return BRUCE_OK;

malformed:
    memory__free(manifest_bytes);
    return BRUCE_ERR_MANIFEST_INVALID;
}

bruce_app_inspection_t *manifest__inspect_javascript(const char *path) {
    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!manifest__normalize_path(path, normalized_path, sizeof(normalized_path))) { return NULL; }

    bruce_app_inspection_t *out_inspection = memory__malloc(sizeof(*out_inspection));
    if (out_inspection == NULL) { return NULL; }
    memset(out_inspection, 0, sizeof(*out_inspection));

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(normalized_path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) {
        memory__free(out_inspection);
        return NULL;
    }

    bruce_manifest_t *parsed = NULL;
    bruce_result_t result;
    char *head = NULL;
    size_t head_len = 0;
    if (!manifest__read_js_head(file, &head, &head_len)) {
        size_t comment_len = 0;
        const char *comment = manifest__extract_js_block_comment(head, head_len, &comment_len);
        if (comment != NULL) { parsed = manifest__parse_js_manifest_comment(comment, comment_len); }
        if (parsed == NULL) { parsed = manifest__default_js_manifest(normalized_path); }
        if (parsed != NULL) {
            out_inspection->kind = BRUCE_APP_KIND_JAVASCRIPT;
            out_inspection->manifest = *parsed;
            out_inspection->abi_warning = out_inspection->manifest.core_abi_version != BRUCE_CORE_ABI_VERSION;
            memory__free(parsed);
            result = BRUCE_OK;
        } else {
            result = BRUCE_ERR_MANIFEST_INVALID;
        }
        memory__free(head);
    } else {
        result = BRUCE_ERR_MANIFEST_INVALID;
    }

    storage__close(file);
    if (result != BRUCE_OK) {
        memory__free(out_inspection);
        return NULL;
    }
    return out_inspection;
}

bruce_app_inspection_t *manifest__inspect_wasm(const char *path) {
    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!manifest__normalize_path(path, normalized_path, sizeof(normalized_path))) { return NULL; }
    size_t path_len = strlen(normalized_path);
    if (path_len <= 5 || strcasecmp(normalized_path + path_len - 5, ".wasm") != 0) { return NULL; }

    bruce_app_inspection_t *out_inspection = memory__malloc(sizeof(*out_inspection));
    if (out_inspection == NULL) { return NULL; }
    memset(out_inspection, 0, sizeof(*out_inspection));

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(normalized_path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) {
        memory__free(out_inspection);
        return NULL;
    }

    char *bytes = NULL;
    size_t bytes_len = 0;
    bool module_valid = false;
    bruce_result_t result = manifest__read_wasm_manifest_bytes(file, &bytes, &bytes_len, &module_valid);
    storage__close(file);

    bruce_manifest_t *manifest = result == BRUCE_OK ? manifest__parse(bytes, bytes_len) : NULL;
    memory__free(bytes);
    if (manifest == NULL && module_valid && result != BRUCE_ERR_NO_MEMORY) {
        manifest = manifest__default_wasm_manifest(normalized_path);
    }
    if (manifest == NULL) {
        memory__free(out_inspection);
        return NULL;
    }

    out_inspection->kind = BRUCE_APP_KIND_WEBASSEMBLY;
    out_inspection->manifest = *manifest;
    out_inspection->abi_warning = out_inspection->manifest.core_abi_version != BRUCE_CORE_ABI_VERSION;
    memory__free(manifest);
    return out_inspection;
}

/* Updates the universal extractor to also recognize JavaScript files. */
static bruce_result_t
manifest__read_js_manifest_bytes(bruce_file_id_t file, char **out_bytes, size_t *out_len) {
    char *head = NULL;
    size_t head_len = 0;
    bruce_result_t result = manifest__read_js_head(file, &head, &head_len);
    if (result != BRUCE_OK) { return result; }

    size_t comment_len = 0;
    const char *comment = manifest__extract_js_block_comment(head, head_len, &comment_len);
    if (comment == NULL) {
        memory__free(head);
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    char *bytes = memory__malloc(comment_len + 1);
    if (bytes == NULL) {
        memory__free(head);
        return BRUCE_ERR_NO_MEMORY;
    }
    memcpy(bytes, comment, comment_len);
    bytes[comment_len] = '\0';
    memory__free(head);
    *out_bytes = bytes;
    *out_len = comment_len;
    return BRUCE_OK;
}
