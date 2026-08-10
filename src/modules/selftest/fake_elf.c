#include "fake_elf.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/storage/storage.h"
#include "core_sdk/manifest.h"

#if CONFIG_IDF_TARGET_ARCH_RISCV
#define SELFTEST_FAKE_ELF_MACHINE 243u /* EM_RISCV, must match elf_loader.c */
#else
#define SELFTEST_FAKE_ELF_MACHINE 94u /* EM_XTENSA, must match elf_loader.c */
#endif

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
} selftest_elf_ehdr_t;

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
} selftest_elf_shdr_t;

/* All-zero-byte input always base64-encodes to runs of 'A' (each 6-bit group
 * is 0), so this needs no real encoder: 42 "AAAA" groups (126 bytes) plus a
 * 2-byte remainder ("AAA=") covers the 128-byte icon this helper always
 * writes. */
static void selftest__fake_elf_icon_base64(char *out, size_t out_capacity) {
    size_t out_index = 0;
    for (int i = 0; i < 42 && out_index + 4 < out_capacity; ++i) {
        memcpy(out + out_index, "AAAA", 4);
        out_index += 4;
    }
    if (out_index + 4 < out_capacity) {
        memcpy(out + out_index, "AAA=", 4);
        out_index += 4;
    }
    out[out_index] = '\0';
}

bool selftest__write_fake_elf(
    const char *path, const char *app_name, const char *const *permissions, size_t permission_count
) {
    char icon_b64[200];
    selftest__fake_elf_icon_base64(icon_b64, sizeof(icon_b64));

    char manifest[512];
    int offset = snprintf(
        manifest,
        sizeof(manifest),
        "{\"appName\":\"%s\",\"appIcon\":\"%s\",\"coreAbiVersion\":%u,\"stackSize\":8192,"
        "\"permissions\":[",
        app_name,
        icon_b64,
        (unsigned)BRUCE_CORE_ABI_VERSION
    );
    for (size_t i = 0; i < permission_count && offset > 0 && (size_t)offset < sizeof(manifest); ++i) {
        offset += snprintf(
            manifest + offset,
            sizeof(manifest) - (size_t)offset,
            "%s\"%s\"",
            i == 0 ? "" : ",",
            permissions[i]
        );
    }
    if (offset > 0 && (size_t)offset < sizeof(manifest)) {
        offset += snprintf(manifest + offset, sizeof(manifest) - (size_t)offset, "]}");
    }
    if (offset <= 0 || (size_t)offset >= sizeof(manifest)) { return false; }
    size_t manifest_len = (size_t)offset;

    /* shstrtab layout: [0]="" [1]=".bruce.manifest\0" [17]=".shstrtab\0" */
    static const char shstr_data[] = "\0.bruce.manifest\0.shstrtab";
    size_t shstr_len = sizeof(shstr_data);

    uint8_t buffer[2048];
    size_t manifest_offset = sizeof(selftest_elf_ehdr_t);
    size_t shstr_offset = manifest_offset + manifest_len;
    size_t shoff = shstr_offset + shstr_len;
    size_t total_size = shoff + 3 * sizeof(selftest_elf_shdr_t);
    if (total_size > sizeof(buffer)) { return false; }

    selftest_elf_ehdr_t ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7f;
    ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';
    ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 1; /* ELFCLASS32 */
    ehdr.e_ident[5] = 1; /* ELFDATA2LSB */
    ehdr.e_ident[6] = 1; /* EV_CURRENT */
    ehdr.e_type = 2;     /* ET_EXEC */
    ehdr.e_machine = (uint16_t)SELFTEST_FAKE_ELF_MACHINE;
    ehdr.e_version = 1;
    ehdr.e_shoff = (uint32_t)shoff;
    ehdr.e_ehsize = sizeof(ehdr);
    ehdr.e_shentsize = sizeof(selftest_elf_shdr_t);
    ehdr.e_shnum = 3;
    ehdr.e_shstrndx = 2;

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, &ehdr, sizeof(ehdr));
    memcpy(buffer + manifest_offset, manifest, manifest_len);
    memcpy(buffer + shstr_offset, shstr_data, shstr_len);

    selftest_elf_shdr_t sh_manifest;
    memset(&sh_manifest, 0, sizeof(sh_manifest));
    sh_manifest.sh_name = 1;  /* ".bruce.manifest" */
    sh_manifest.sh_type = 1;  /* SHT_PROGBITS */
    sh_manifest.sh_flags = 0; /* not SHF_ALLOC -> non-loadable */
    sh_manifest.sh_offset = (uint32_t)manifest_offset;
    sh_manifest.sh_size = (uint32_t)manifest_len;

    selftest_elf_shdr_t sh_shstrtab;
    memset(&sh_shstrtab, 0, sizeof(sh_shstrtab));
    sh_shstrtab.sh_name = 17; /* ".shstrtab" */
    sh_shstrtab.sh_type = 3;  /* SHT_STRTAB */
    sh_shstrtab.sh_offset = (uint32_t)shstr_offset;
    sh_shstrtab.sh_size = (uint32_t)shstr_len;

    /* buffer's leading section (index 0) stays the required all-zero NULL
     * section header (already zeroed above). */
    memcpy(buffer + shoff + sizeof(selftest_elf_shdr_t), &sh_manifest, sizeof(sh_manifest));
    memcpy(buffer + shoff + 2 * sizeof(selftest_elf_shdr_t), &sh_shstrtab, sizeof(sh_shstrtab));

    return storage__write_file_atomic(path, buffer, total_size);
}
