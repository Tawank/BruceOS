#include "core_sdk/manifest.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core_sdk/permission.h"
#include "core_sdk/storage.h"

/* ----------------------------------------------------------------------- */
/* JSON manifest parser (shared)                                            */
/* ----------------------------------------------------------------------- */

#define MANIFEST__STACK_MIN 4096u
#define MANIFEST__STACK_MAX 16384u

static int manifest__base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool manifest__base64_decode_exact(const char *in, uint8_t *out, size_t out_size)
{
    size_t in_len = strlen(in);
    if (in_len == 0 || in_len % 4 != 0) {
        return false;
    }

    size_t pad = 0;
    if (in[in_len - 1] == '=') pad++;
    if (in_len >= 2 && in[in_len - 2] == '=') pad++;

    size_t decoded_len = (in_len / 4) * 3 - pad;
    if (decoded_len != out_size) {
        return false;
    }

    size_t out_index = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        char c2 = in[i + 2];
        char c3 = in[i + 3];
        int v0 = manifest__base64_value(in[i]);
        int v1 = manifest__base64_value(in[i + 1]);
        int v2 = (c2 == '=') ? 0 : manifest__base64_value(c2);
        int v3 = (c3 == '=') ? 0 : manifest__base64_value(c3);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            return false;
        }

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        if (out_index < out_size) out[out_index++] = (uint8_t)(triple >> 16);
        if (c2 != '=' && out_index < out_size) out[out_index++] = (uint8_t)(triple >> 8);
        if (c3 != '=' && out_index < out_size) out[out_index++] = (uint8_t)triple;
    }
    return out_index == out_size;
}

bruce_result_t manifest__parse(const char *json, size_t json_len, bruce_manifest_t *out_manifest)
{
    if (json == NULL || out_manifest == NULL || json_len == 0) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return BRUCE_ERR_MANIFEST_INVALID;
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
        cJSON_Delete(root);
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    if (!manifest__base64_decode_exact(icon->valuestring, out_manifest->app_icon, BRUCE_MANIFEST_ICON_BYTES)) {
        cJSON_Delete(root);
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    strncpy(out_manifest->app_name, name->valuestring, BRUCE_MANIFEST_APP_NAME_MAX - 1);
    out_manifest->core_abi_version = (uint32_t)abi->valuedouble;
    out_manifest->stack_size = (uint32_t)stack->valuedouble;

    size_t permission_count = 0;
    if (permissions != NULL) {
        int array_size = cJSON_GetArraySize(permissions);
        if (array_size < 0 || (size_t)array_size > BRUCE_MANIFEST_MAX_PERMISSIONS) {
            cJSON_Delete(root);
            return BRUCE_ERR_MANIFEST_INVALID;
        }
        for (int i = 0; i < array_size; ++i) {
            const cJSON *entry = cJSON_GetArrayItem(permissions, i);
            bruce_permission_t permission;
            if (!cJSON_IsString(entry) || entry->valuestring == NULL ||
                !permission__from_name(entry->valuestring, &permission)) {
                cJSON_Delete(root);
                return BRUCE_ERR_MANIFEST_INVALID;
            }
            bool duplicate = false;
            for (size_t j = 0; j < permission_count; ++j) {
                if (strcmp(out_manifest->permissions[j], entry->valuestring) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate || strlen(entry->valuestring) >= BRUCE_MANIFEST_PERMISSION_NAME_MAX) {
                cJSON_Delete(root);
                return BRUCE_ERR_MANIFEST_INVALID;
            }
            strncpy(out_manifest->permissions[permission_count], entry->valuestring,
                    BRUCE_MANIFEST_PERMISSION_NAME_MAX - 1);
            permission_count++;
        }
    }
    out_manifest->permission_count = permission_count;

    cJSON_Delete(root);
    return BRUCE_OK;
}

/* ----------------------------------------------------------------------- */
/* File-I/O helpers for format detection & manifest extraction              */
/* ----------------------------------------------------------------------- */

static bool manifest__pread(bruce_file_id_t file, uint64_t offset, void *buffer, size_t size)
{
    if (storage__seek(file, (int64_t)offset, SEEK_SET, NULL) != BRUCE_OK) {
        return false;
    }
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
/* ELF-specific manifest extraction                                         */
/* ----------------------------------------------------------------------- */

/* This build's expected e_machine value (see migration_BruceIDF.md, "ELF
 * contract": "The ELF header, not the manifest, is authoritative for
 * architecture").  ESP-IDF defines exactly one of these two Kconfig symbols
 * per target. */
#if CONFIG_IDF_TARGET_ARCH_RISCV
#define MANIFEST_ELF_EXPECTED_MACHINE 243u /* EM_RISCV */
#else
#define MANIFEST_ELF_EXPECTED_MACHINE 94u /* EM_XTENSA */
#endif

#define MANIFEST_ELF_SECTION_NAME ".bruce.manifest"
#define MANIFEST_ELF_SHF_ALLOC 0x2u
#define MANIFEST_ELF_MAX_MANIFEST_BYTES 2048u

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

static bruce_result_t manifest__inspect_elf(bruce_file_id_t file, bruce_app_inspection_t *out_inspection)
{
    manifest_elf_ehdr_t header;
    if (!manifest__pread(file, 0, &header, sizeof(header))) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }
    if (memcmp(header.e_ident, "\x7f" "ELF", 4) != 0 || header.e_ident[4] != 1 /* ELFCLASS32 */) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }
    if (header.e_machine != MANIFEST_ELF_EXPECTED_MACHINE) {
        return BRUCE_ERR_TARGET_MISMATCH;
    }
    if (header.e_shnum == 0 || header.e_shstrndx >= header.e_shnum) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    manifest_elf_shdr_t shstrtab_hdr;
    if (!manifest__pread(file, header.e_shoff + (uint64_t)header.e_shstrndx * header.e_shentsize, &shstrtab_hdr,
                          sizeof(shstrtab_hdr))) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    for (uint16_t i = 0; i < header.e_shnum; ++i) {
        manifest_elf_shdr_t section;
        if (!manifest__pread(file, header.e_shoff + (uint64_t)i * header.e_shentsize, &section, sizeof(section))) {
            return BRUCE_ERR_MANIFEST_INVALID;
        }

        char name[sizeof(MANIFEST_ELF_SECTION_NAME)];
        if (!manifest__pread(file, shstrtab_hdr.sh_offset + section.sh_name, name, sizeof(name))) {
            continue;
        }
        name[sizeof(name) - 1] = '\0';
        if (strcmp(name, MANIFEST_ELF_SECTION_NAME) != 0) {
            continue;
        }
        if ((section.sh_flags & MANIFEST_ELF_SHF_ALLOC) != 0 || section.sh_size == 0 ||
            section.sh_size > MANIFEST_ELF_MAX_MANIFEST_BYTES) {
            return BRUCE_ERR_MANIFEST_INVALID;
        }

        char *bytes = malloc(section.sh_size);
        if (bytes == NULL) {
            return BRUCE_ERR_NO_MEMORY;
        }
        bool read_ok = manifest__pread(file, section.sh_offset, bytes, section.sh_size);
        bruce_result_t parse_result = read_ok ? manifest__parse(bytes, section.sh_size, &out_inspection->manifest)
                                               : BRUCE_ERR_MANIFEST_INVALID;
        free(bytes);
        if (parse_result != BRUCE_OK) {
            return parse_result;
        }

        out_inspection->kind = BRUCE_APP_KIND_ELF;
        out_inspection->abi_warning = out_inspection->manifest.core_abi_version != BRUCE_CORE_ABI_VERSION;
        return BRUCE_OK;
    }

    return BRUCE_ERR_MANIFEST_INVALID;
}

/* ----------------------------------------------------------------------- */
/* Universal manifest inspection                                            */
/* ----------------------------------------------------------------------- */

bruce_result_t manifest__inspect_path(const char *path, bruce_app_inspection_t *out_inspection)
{
    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL || out_inspection == NULL) {
        return BRUCE_ERR_INVALID_PATH;
    }
    memset(out_inspection, 0, sizeof(*out_inspection));

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) {
        return open_result;
    }

    uint8_t magic[4];
    bruce_result_t result;
    if (manifest__pread(file, 0, magic, sizeof(magic)) && memcmp(magic, "\x7f" "ELF", 4) == 0) {
        result = manifest__inspect_elf(file, out_inspection);
    } else {
        result = BRUCE_ERR_MANIFEST_INVALID;
    }

    storage__close(file);
    return result;
}
