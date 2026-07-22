#include "elf_loader.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/storage.h"

/* This build's expected e_machine value (see migration_BruceIDF.md, "ELF
 * contract": "The ELF header, not the manifest, is authoritative for
 * architecture"). ESP-IDF defines exactly one of these two Kconfig symbols
 * per target. */
#if CONFIG_IDF_TARGET_ARCH_RISCV
#define ELF_LOADER_EXPECTED_MACHINE 243u /* EM_RISCV */
#else
#define ELF_LOADER_EXPECTED_MACHINE 94u /* EM_XTENSA */
#endif

#define ELF_LOADER_SECTION_NAME ".bruce.manifest"
#define ELF_LOADER_SHF_ALLOC 0x2u
#define ELF_LOADER_MAX_MANIFEST_BYTES 2048u

/* Elf32_Ehdr / Elf32_Shdr, hand-declared (packed) so field offsets match the
 * on-disk format regardless of host struct-packing defaults. */
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
} elf_loader_ehdr_t;

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
} elf_loader_shdr_t;

static size_t s_call_count;

size_t elf_loader__debug_call_count(void)
{
    return s_call_count;
}

static bool elf_loader__path_is_valid(const char *path)
{
    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL) {
        return false;
    }
    size_t length = strlen(path);
    static const char extension[] = ".elf";
    size_t extension_length = sizeof(extension) - 1;
    return length > extension_length && strcmp(path + length - extension_length, extension) == 0;
}

static const char *elf_loader__basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* Reads exactly `size` bytes at `offset` from the already-open `file`.
 * Returns false on a short read or any I/O error. */
static bool elf_loader__pread(bruce_file_id_t file, uint64_t offset, void *buffer, size_t size)
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

/* Locates the non-loadable ".bruce.manifest" section, reads its raw bytes,
 * and hands them to the shared manifest__parse(); also validates e_machine
 * against this build's target architecture.  `file` must already be open
 * for read. */
static bruce_result_t elf_loader__parse_from_open_file(bruce_file_id_t file, bruce_app_inspection_t *out_inspection)
{
    elf_loader_ehdr_t header;
    if (!elf_loader__pread(file, 0, &header, sizeof(header))) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }
    if (memcmp(header.e_ident, "\x7f" "ELF", 4) != 0 || header.e_ident[4] != 1 /* ELFCLASS32 */) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }
    if (header.e_machine != ELF_LOADER_EXPECTED_MACHINE) {
        return BRUCE_ERR_TARGET_MISMATCH;
    }
    if (header.e_shnum == 0 || header.e_shstrndx >= header.e_shnum) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    elf_loader_shdr_t shstrtab_hdr;
    if (!elf_loader__pread(file, header.e_shoff + (uint64_t)header.e_shstrndx * header.e_shentsize, &shstrtab_hdr,
                            sizeof(shstrtab_hdr))) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    for (uint16_t i = 0; i < header.e_shnum; ++i) {
        elf_loader_shdr_t section;
        if (!elf_loader__pread(file, header.e_shoff + (uint64_t)i * header.e_shentsize, &section, sizeof(section))) {
            return BRUCE_ERR_MANIFEST_INVALID;
        }

        char name[sizeof(ELF_LOADER_SECTION_NAME)];
        if (!elf_loader__pread(file, shstrtab_hdr.sh_offset + section.sh_name, name, sizeof(name))) {
            continue;
        }
        name[sizeof(name) - 1] = '\0';
        if (strcmp(name, ELF_LOADER_SECTION_NAME) != 0) {
            continue;
        }
        if ((section.sh_flags & ELF_LOADER_SHF_ALLOC) != 0 || section.sh_size == 0 ||
            section.sh_size > ELF_LOADER_MAX_MANIFEST_BYTES) {
            return BRUCE_ERR_MANIFEST_INVALID;
        }

        char *bytes = malloc(section.sh_size);
        if (bytes == NULL) {
            return BRUCE_ERR_NO_MEMORY;
        }
        bool read_ok = elf_loader__pread(file, section.sh_offset, bytes, section.sh_size);
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

    return BRUCE_ERR_MANIFEST_INVALID; /* every ELF must contain the section */
}

static bruce_result_t elf_loader__inspect_path(const char *path, bruce_app_inspection_t *out_inspection)
{
    if (!elf_loader__path_is_valid(path) || out_inspection == NULL) {
        return BRUCE_ERR_INVALID_PATH;
    }
    memset(out_inspection, 0, sizeof(*out_inspection));

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) {
        return open_result;
    }
    bruce_result_t result = elf_loader__parse_from_open_file(file, out_inspection);
    storage__close(file);
    return result;
}

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];
    char permission_key[BRUCE_PERMISSION_FILE_NAME_MAX];
} elf_loader_task_ctx_t;

/* Real relocation/execution needs an actual ELF loader library integration,
 * which agent_tasks.md (A6) leaves as remaining work.  For now this proves
 * the rest of the pipeline (manifest validation, permission preflight, task
 * spawn, tracked image memory) by loading the image into a memory__malloc()
 * buffer - freed automatically by the resource registry even though this
 * exits without running it. */
static void elf_loader__task_entry(void *context)
{
    elf_loader_task_ctx_t *ctx = (elf_loader_task_ctx_t *)context;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(ctx->path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        uint64_t size = 0;
        if (storage__seek(file, 0, SEEK_END, &size) == BRUCE_OK && size > 0) {
            void *image = memory__malloc((size_t)size);
            if (image != NULL) {
                (void)elf_loader__pread(file, 0, image, (size_t)size);
                memory__free(image);
            }
        }
        storage__close(file);
    }

    printf("[elf_loader] %s: execution not implemented yet (registry/manifest validation only)\n",
           ctx->permission_key);
    free(ctx);
}

static int elf_loader__run_path(const char *path, const char *arg, bool in_background)
{
    s_call_count++;

    if (!elf_loader__path_is_valid(path)) {
        return BRUCE_ERR_INVALID_PATH;
    }

    bruce_app_inspection_t inspection;
    bruce_result_t inspect_result = elf_loader__inspect_path(path, &inspection);
    if (inspect_result != BRUCE_OK) {
        return (int)inspect_result;
    }

    const char *permission_key = elf_loader__basename(path);

    const char *permission_names[BRUCE_MANIFEST_MAX_PERMISSIONS];
    for (size_t i = 0; i < inspection.manifest.permission_count; ++i) {
        permission_names[i] = inspection.manifest.permissions[i];
    }
    (void)permission__preflight(permission_key, permission_names, inspection.manifest.permission_count);

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) {
        return (int)parse_result;
    }
    bool gui_requested = app_runner__args_have_gui(argc, argv);
    app_runner__free_args(argv, argc);

    elf_loader_task_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return BRUCE_ERR_NO_MEMORY;
    }
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    ctx->path[sizeof(ctx->path) - 1] = '\0';
    strncpy(ctx->permission_key, permission_key, sizeof(ctx->permission_key) - 1);
    ctx->permission_key[sizeof(ctx->permission_key) - 1] = '\0';

    int result = app_runner__spawn_loader_task(permission_key, gui_requested, in_background,
                                                inspection.manifest.stack_size, elf_loader__task_entry, ctx);
    if (result <= 0) {
        free(ctx);
    }
    return result;
}

void elf_loader__register(void)
{
    (void)app_runner__register_loader(".elf", 10, elf_loader__run_path, elf_loader__inspect_path);
}
