/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/param.h>

#include "esp_elf.h"
#include "esp_log.h"

#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#include "hal/cache_ll.h"
#endif

#include "private/elf_platform.h"

#define stype(_s, _t) ((_s)->type == (_t))
#define sflags(_s, _f) (((_s)->flags & (_f)) == (_f))
#define ADDR_OFFSET (0x400)

#ifdef CONFIG_ELF_LOADER_NUMBER_SYMBOLS
#define SYMBOL_TABLES_NO CONFIG_ELF_LOADER_NUMBER_SYMBOLS
#else
#define SYMBOL_TABLES_NO (32)
#endif

#ifdef CONFIG_ELF_FILE_SYSTEM_BASE_PATH
#define FS_PATH CONFIG_ELF_FILE_SYSTEM_BASE_PATH
#else
#define FS_PATH "/storage"
#endif

static const char *TAG = "ELF";

#if CONFIG_BRUCE_QEMU_TEST_MODE
/* See esp_elf.h's doc comment on esp_elf_debug_force_streaming_fallback(). */
static bool s_debug_force_streaming_fallback = false;

void esp_elf_debug_force_streaming_fallback(bool force) { s_debug_force_streaming_fallback = force; }
#endif
static esp_elf_symbol_table_t *g_symbol_tables[SYMBOL_TABLES_NO];
static _Atomic(symbol_resolver) current_resolver = elf_find_sym_default;

/**
 * @brief Open and load an ELF file into memory.
 *
 * @param file - Pointer to elf_file_t structure to store loaded file content
 * @param name - Filename (without path) of the ELF file to open
 *
 * @return 0 on success, -1 on failure with errno set. Error cases include:
 *         - Invalid parameters
 *         - Path generation failure
 *         - File open/read errors
 *         - Memory allocation failures
 *
 * @note The actual file path will be constructed as "FS_PATH/name"
 * @note Allocates memory for file content using esp_elf_malloc()
 */
int esp_elf_open(elf_file_t *file, const char *name) {
    ssize_t ret;
    int fd;
    char *file_path;
    off_t size;
    uint8_t *pbuf;

    if (!file || !name) {
        errno = EINVAL;
        return -1;
    }

    ret = asprintf(&file_path, FS_PATH "/%s", name);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to generate path errno=%d", errno);
        return -1;
    }

    fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open file %s errno=%d", file_path, errno);
        goto errout_open_file;
    }

    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        ESP_LOGE(TAG, "Failed to seek file %s errno=%d", file_path, errno);
        goto errout_lseek_end;
    }

    ret = lseek(fd, 0, SEEK_SET);
    if (ret == -1) {
        ESP_LOGE(TAG, "Failed to seek file %s errno=%d", file_path, errno);
        goto errout_lseek_end;
    }

    pbuf = esp_elf_malloc(size, false);
    if (!pbuf) {
        ESP_LOGE(TAG, "Failed to malloc %" PRId64 " bytes", (int64_t)size);
        goto errout_lseek_end;
    }

    ret = read(fd, pbuf, size);
    if (ret != (ssize_t)size) {
        ESP_LOGE(TAG, "Failed to read ret=%zd", ret);
        goto errout_read_fs;
    }

    free(file_path);
    close(fd);
    file->payload = pbuf;
    file->size = size;

    return 0;

errout_read_fs:
    esp_elf_free(pbuf);
errout_lseek_end:
    close(fd);
errout_open_file:
    free(file_path);
    return -1;
}

/**
 * @brief Close ELF file and release associated resources.
 *
 * @param file - Pointer to opened elf_file_t structure
 *
 * @note Releases memory allocated by esp_elf_open() for payload data
 * @note Should be called paired with esp_elf_open() to prevent memory leaks
 * @note If file is NULL, this function does nothing (null-safe)
 */
void esp_elf_close(elf_file_t *file) {
    if (!file) { return; }

    esp_elf_free(file->payload);
    file->payload = NULL;
    file->size = 0;
}

/**
 * @brief Find symbol address by name.
 *
 * @param sym_name - Symbol name
 *
 * @return Symbol address if success or 0 if failed.
 */
uintptr_t elf_find_sym(const char *sym_name) {
    if (!sym_name) {
        ESP_LOGE(TAG, "Invalid parameter: sym_name is NULL");
        return 0;
    }

    symbol_resolver resolver = atomic_load(&current_resolver);
    return resolver(sym_name);
}

/**
 * @brief Resolves the symbol address one RELA entry's relocation needs,
 * exactly as esp_elf_relocate_internal()'s own top-level relocation loop
 * always has (STT_COMMON/OBJECT/SECTION -> named lookup,
 * STT_FILE -> symbol-value-or-named-lookup, anything else -> no lookup
 * needed, addr stays 0). Factored out so esp_elf_stream_text_to_xip()'s
 * low-memory streaming fallback (below) resolves relocations exactly the
 * same way that loop does, instead of a second, drift-prone copy of this
 * logic.
 *
 * @param elf     - ELF object pointer
 * @param rela    - Relocation entry needing a symbol address
 * @param sym     - This relocation's symbol table entry
 * @param strtab  - String table `sym->name` indexes into
 * @param out_addr - Set to the resolved address (0 if none was needed)
 *
 * @return 0 on success, -ENOSYS if a required symbol could not be found
 */
static int esp_elf_resolve_relocation_addr(
    esp_elf_t *elf, const elf32_rela_t *rela, const elf32_sym_t *sym, const char *strtab, uintptr_t *out_addr
) {
    *out_addr = 0;
    int type = ELF_R_TYPE(rela->info);

    if (type == STT_COMMON || type == STT_OBJECT || type == STT_SECTION) {
        const char *comm_name = strtab + sym->name;

        if (comm_name[0]) {
            *out_addr = elf_find_sym(comm_name);
#if CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
            if (!*out_addr && sym->shndx != SHN_UNDEF) {
#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
                *out_addr =
                    (uintptr_t)(elf->sec[ELF_SEC_DATA].addr + sym->value - elf->sec[ELF_SEC_DATA].v_addr);
#else
                *out_addr = (uintptr_t)(elf->psegment + sym->value - elf->svaddr);
#endif
            }
#endif
            if (!*out_addr) {
                ESP_LOGE(TAG, "Can't find common %s", strtab + sym->name);
                return -ENOSYS;
            }
            ESP_LOGD(TAG, "Find common %s addr=%x", comm_name, (unsigned int)*out_addr);
        }
    } else if (type == STT_FILE) {
        const char *func_name = strtab + sym->name;

        if (sym->value) {
            *out_addr = esp_elf_map_sym(elf, sym->value);
        } else {
            *out_addr = elf_find_sym(func_name);
        }
#if CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
        if (!*out_addr && sym->shndx != SHN_UNDEF) {
#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
            *out_addr = (uintptr_t)(elf->sec[ELF_SEC_TEXT].addr + sym->value - elf->sec[ELF_SEC_TEXT].v_addr);
#else
            *out_addr = (uintptr_t)(elf->psegment + sym->value - elf->svaddr);
#endif
        }
#endif
        if (!*out_addr) {
            ESP_LOGE(TAG, "Can't find symbol %s", func_name);
            return -ENOSYS;
        }
        ESP_LOGD(TAG, "Find function %s addr=%x", func_name, (unsigned int)*out_addr);
    }

    return 0;
}

#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR

#define ESP_ELF_XIP_STREAM_WINDOW 4096u

bool esp_elf_target_streamed(esp_elf_t *elf, uintptr_t offset, size_t *out_target) {
    uint32_t text_v_min = elf->sec[ELF_SEC_TEXT].v_addr;
    size_t text_size = elf->sec[ELF_SEC_TEXT].size;

    if (offset >= text_v_min && offset < text_v_min + text_size) {
        if (out_target) *out_target = offset - text_v_min;
        return true;
    }
    if (elf->xip_rodata_staged && elf->sec[ELF_SEC_RODATA].size &&
        offset >= elf->sec[ELF_SEC_RODATA].v_addr &&
        offset < elf->sec[ELF_SEC_RODATA].v_addr + elf->sec[ELF_SEC_RODATA].size) {
        /* .rodata lands at elf->xip_rodata_offset in the xip/swap buffer,
         * NOT at text_size -- xip_rodata_offset is text_size rounded up to
         * a 4-byte boundary (see its ELF_ALIGN() at the top of
         * esp_elf_load_section()), which is what sec[ELF_SEC_RODATA].addr,
         * the ptext-based copy, and the final flash write offset all use.
         * Splitting at the unaligned text_size instead would put these
         * bytes 1-3 bytes earlier than every other consumer expects them,
         * corrupting .rodata whenever text_size isn't already 4-aligned. */
        if (out_target) *out_target = elf->xip_rodata_offset + (offset - elf->sec[ELF_SEC_RODATA].v_addr);
        return true;
    }
    return false;
}

/**
 * @brief Streams the relocated ".text" (and ".iram1.*"/staged ".rodata", see
 * esp_elf_load_section()) straight to a swap/XIP target ESP_ELF_XIP_STREAM_WINDOW
 * bytes at a time, instead of materializing it all in one elf->ptext buffer
 * first. Used as a fallback by esp_elf_load_section() when esp_elf_malloc()
 * can't find one contiguous internal-RAM block that big -- total free
 * memory looking fine says nothing about whether a block that size actually
 * exists (see the "free"/"lrgst" columns of the `free` CLI command); a
 * fragmented heap can starve this allocation even with plenty of memory
 * free overall, and the ELF itself may simply be too big for internal RAM
 * on a board with no PSRAM either way.
 *
 * This is safe -- not just "workable" -- because of how narrow the set of
 * relocation types this loader supports for a TEXT/staged-RODATA target
 * actually is (see esp_elf_arch_relocate_at()): R_XTENSA_RTLD does nothing;
 * R_XTENSA_GLOB_DAT/JMP_SLOT write a value that's already fully resolved,
 * no read involved; R_XTENSA_RELATIVE reads *where* first, but only to
 * recover the raw, pre-relocation vaddr placeholder the compiler originally
 * embedded there -- and since every relocation entry targets a distinct
 * offset (nothing here ever relocates the same 4 bytes twice), that
 * placeholder is always still exactly what sits at the matching offset in
 * `pbuf`, untouched, regardless of what order relocations run in or
 * whether any other relocation has run yet. So every window can be built
 * from `pbuf` plus only the relocations landing inside it, with no
 * dependency on any other window -- there's no need to see the whole
 * section at once, just ESP_ELF_XIP_STREAM_WINDOW bytes of it.
 *
 * Each window is written to swap exactly once, already in its final
 * relocated form. That matters for more than RAM: swap writes are cheap
 * (no flash erase needed) only the first time a given byte is written --
 * writing raw bytes first and going back to patch relocations in
 * afterwards would make most of those patches require reading back,
 * erasing, and rewriting the whole enclosing 4KB flash sector (see
 * memory_external.c's memory_external__write()), for every single
 * relocation that doesn't happen to only clear bits. Building each window
 * fully in RAM before its one write avoids that entirely.
 *
 * elf->xip_ops->allocate() (and therefore elf->xip_data/xip_handle and
 * elf->sec[ELF_SEC_TEXT/RODATA].addr) must already have run by the time
 * this is called -- same precondition esp_elf_load_section()'s normal path
 * has by the time it reaches relocation, just reordered earlier for this
 * fallback (see the call site). elf->ptext is left NULL; it's never
 * allocated here at all. esp_elf_arch_relocate() (the entry point the later
 * top-level relocation loop actually calls) knows a NULL esp_elf_map_reloc()
 * result for a TEXT/staged-RODATA target combined with elf->ptext == NULL
 * means "already handled here", not a real error -- see its own comment.
 *
 * @param elf            - ELF object pointer (xip_ops/xip_handle/xip_data and
 *                          sec[ELF_SEC_TEXT]/[ELF_SEC_RODATA] already set)
 * @param pbuf           - ELF data buffer
 * @param ehdr           - ELF header (within pbuf)
 * @param shdr           - ELF section header table (within pbuf)
 * @param text_work_size - Total bytes to stream: elf->sec[ELF_SEC_TEXT].size,
 *                          plus elf->sec[ELF_SEC_RODATA].size too if xip_rodata_staged
 *
 * @return 0 on success, a negative errno on failure (same conventions as
 *         esp_elf_malloc()/xip_ops failures elsewhere in this file)
 */
static int esp_elf_stream_text_to_xip(
    esp_elf_t *elf, const uint8_t *pbuf, const elf32_hdr_t *ehdr, const elf32_shdr_t *shdr,
    size_t text_work_size
) {
    uint32_t text_v_min = elf->sec[ELF_SEC_TEXT].v_addr;
    size_t text_size = elf->sec[ELF_SEC_TEXT].size;

    uint8_t *window = esp_elf_malloc(ESP_ELF_XIP_STREAM_WINDOW, false);
    if (!window) {
        ESP_LOGE(TAG, "Failed to malloc %u bytes for the streaming fallback window", ESP_ELF_XIP_STREAM_WINDOW);
        return -ENOMEM;
    }

    int ret = 0;
    for (size_t window_start = 0; window_start < text_work_size; window_start += ESP_ELF_XIP_STREAM_WINDOW) {
        size_t window_size = text_work_size - window_start;
        if (window_size > ESP_ELF_XIP_STREAM_WINDOW) window_size = ESP_ELF_XIP_STREAM_WINDOW;
        size_t window_end = window_start + window_size;

        /* Fill with raw, pre-relocation bytes -- the same sources
         * esp_elf_load_section()'s own memcpy(s) use for this data, just
         * clipped to this window. [0, text_size) is ".text"/".iram1.*";
         * [xip_rodata_offset, text_work_size) -- only reachable when
         * xip_rodata_staged -- is ".rodata", starting at the same
         * 4-byte-aligned offset every other consumer of this buffer uses
         * (see esp_elf_target_streamed()'s comment). Any bytes in
         * [text_size, xip_rodata_offset) are alignment padding. */
        if (window_start < text_size) {
            for (uint32_t i = 0; i < ehdr->shnum; i++) {
                if (!(stype(&shdr[i], SHT_PROGBITS) && sflags(&shdr[i], SHF_ALLOC) &&
                      sflags(&shdr[i], SHF_EXECINSTR))) {
                    continue;
                }
                size_t sec_start = shdr[i].addr - text_v_min;
                size_t sec_end = sec_start + shdr[i].size;
                size_t copy_start = sec_start > window_start ? sec_start : window_start;
                size_t copy_end = sec_end < window_end ? sec_end : window_end;
                if (copy_start >= copy_end) continue;
                memcpy(
                    window + (copy_start - window_start), pbuf + shdr[i].offset + (copy_start - sec_start),
                    copy_end - copy_start
                );
            }
        }
        if (window_end > elf->xip_rodata_offset && elf->xip_rodata_staged) {
            /* .rodata starts at elf->xip_rodata_offset (text_size rounded up
             * to 4 bytes), matching every other consumer of this layout --
             * see esp_elf_target_streamed()'s comment. Bytes in
             * [text_size, xip_rodata_offset), if any, are alignment padding
             * and are left untouched here, same as any other inter-section
             * gap in this window. */
            size_t copy_start = window_start > elf->xip_rodata_offset ? window_start : elf->xip_rodata_offset;
            size_t rodata_offset = copy_start - elf->xip_rodata_offset;
            memcpy(
                window + (copy_start - window_start), pbuf + elf->sec[ELF_SEC_RODATA].offset + rodata_offset,
                window_end - copy_start
            );
        }

        /* Apply every relocation landing in this window, on top of the raw
         * bytes just copied in -- see this function's doc comment for why
         * that's safe regardless of window order. */
        for (uint32_t i = 0; i < ehdr->shnum && ret == 0; i++) {
            if (!stype(&shdr[i], SHT_RELA)) continue;

            uint32_t nr_reloc = shdr[i].size / sizeof(elf32_rela_t);
            const elf32_rela_t *rela = (const elf32_rela_t *)(pbuf + shdr[i].offset);
            const elf32_sym_t *symtab = (const elf32_sym_t *)(pbuf + shdr[shdr[i].link].offset);
            const char *strtab = (const char *)(pbuf + shdr[shdr[shdr[i].link].link].offset);

            for (uint32_t j = 0; j < nr_reloc; j++) {
                elf32_rela_t rela_buf;
                memcpy(&rela_buf, &rela[j], sizeof(elf32_rela_t));

                size_t target;
                if (!esp_elf_target_streamed(elf, rela_buf.offset, &target)) {
                    continue; /* .data/.bss/etc -- handled by the normal pdata-based pass. */
                }
                if (target < window_start || target + sizeof(uint32_t) > window_end) continue;

                const elf32_sym_t *sym = &symtab[ELF_R_SYM(rela_buf.info)];
                uintptr_t addr = 0;
                ret = esp_elf_resolve_relocation_addr(elf, &rela_buf, sym, strtab, &addr);
                if (ret != 0) break;
                ret = esp_elf_arch_relocate_at(
                    elf, &rela_buf, sym, addr, (uint32_t *)(window + (target - window_start))
                );
                if (ret != 0) break;
            }
        }
        if (ret != 0) break;

        ret = elf->xip_ops->write(elf->xip_context, elf->xip_handle, window_start, window, window_size);
        if (ret != 0) break;
    }

    esp_elf_free(window);
    return ret;
}

/**
 * @brief Load ELF section.
 *
 * @param elf - ELF object pointer
 * @param pbuf - ELF data buffer
 *
 * @return ESP_OK if success or other if failed.
 */
static int esp_elf_load_section(esp_elf_t *elf, const uint8_t *pbuf) {
    uint32_t entry;
    uint32_t size;

    const elf32_hdr_t *ehdr = (const elf32_hdr_t *)pbuf;
    const elf32_shdr_t *shdr = (const elf32_shdr_t *)(pbuf + ehdr->shoff);
    const char *shstrab = (const char *)pbuf + shdr[ehdr->shstrndx].offset;
    uint32_t rodata_index = ehdr->shnum;
    uint32_t text_v_min = 0;
    uint32_t text_v_max = 0;
    bool text_found = false;

    /* Calculate ELF image size */

    for (uint32_t i = 0; i < ehdr->shnum; i++) {
        const char *name = shstrab + shdr[i].name;

        if (stype(&shdr[i], SHT_PROGBITS) && sflags(&shdr[i], SHF_ALLOC)) {
            if (sflags(&shdr[i], SHF_EXECINSTR)) {
                /* Every allocated, executable PROGBITS section belongs in
                 * the code region: ".text" itself, plus whatever the
                 * toolchain splits out for IRAM_ATTR placement
                 * (".iram1.N" code and ".iram1.N.literal" L32R pools).
                 * Track them as one contiguous run so a single writable
                 * staging buffer (ptext) covers all of them and every
                 * relocation landing inside one stays mappable -- treating
                 * only the literal ".text" section as "text" left those
                 * IRAM sections unmapped and their relocations failed with
                 * "relocation target is not writable". */
                ESP_LOGD(
                    TAG,
                    "%s sec addr=0x%08x size=0x%08x offset=0x%08x",
                    name,
                    shdr[i].addr,
                    shdr[i].size,
                    shdr[i].offset
                );

                if (!text_found || shdr[i].addr < text_v_min) { text_v_min = shdr[i].addr; }
                if (!text_found || shdr[i].addr + shdr[i].size > text_v_max) {
                    text_v_max = shdr[i].addr + shdr[i].size;
                }
                text_found = true;
            } else if (sflags(&shdr[i], SHF_WRITE) && !strcmp(ELF_DATA, name)) {
                ESP_LOGD(
                    TAG,
                    ".data   sec addr=0x%08x size=0x%08x offset=0x%08x",
                    shdr[i].addr,
                    shdr[i].size,
                    shdr[i].offset
                );

                elf->sec[ELF_SEC_DATA].v_addr = shdr[i].addr;
                elf->sec[ELF_SEC_DATA].size = shdr[i].size;
                elf->sec[ELF_SEC_DATA].offset = shdr[i].offset;

                ESP_LOGD(
                    TAG,
                    ".data   offset is 0x%lx size is 0x%x",
                    elf->sec[ELF_SEC_DATA].offset,
                    elf->sec[ELF_SEC_DATA].size
                );
            } else if (!strcmp(ELF_RODATA, name)) {
                rodata_index = i;
                ESP_LOGD(
                    TAG,
                    ".rodata sec addr=0x%08x size=0x%08x offset=0x%08x",
                    shdr[i].addr,
                    shdr[i].size,
                    shdr[i].offset
                );

                elf->sec[ELF_SEC_RODATA].v_addr = shdr[i].addr;
                elf->sec[ELF_SEC_RODATA].size = shdr[i].size;
                elf->sec[ELF_SEC_RODATA].offset = shdr[i].offset;

                ESP_LOGD(
                    TAG,
                    ".rodata offset is 0x%lx size is 0x%x",
                    elf->sec[ELF_SEC_RODATA].offset,
                    elf->sec[ELF_SEC_RODATA].size
                );
            } else if (!strcmp(ELF_DATA_REL_RO, name)) {
                ESP_LOGD(
                    TAG,
                    ".data.rel.ro sec addr=0x%08x size=0x%08x offset=0x%08x",
                    shdr[i].addr,
                    shdr[i].size,
                    shdr[i].offset
                );

                elf->sec[ELF_SEC_DRLRO].v_addr = shdr[i].addr;
                elf->sec[ELF_SEC_DRLRO].size = shdr[i].size;
                elf->sec[ELF_SEC_DRLRO].offset = shdr[i].offset;

                ESP_LOGD(
                    TAG,
                    ".data.rel.ro offset is 0x%lx size is 0x%x",
                    elf->sec[ELF_SEC_DRLRO].offset,
                    elf->sec[ELF_SEC_DRLRO].size
                );
            } else if (!strcmp(ELF_CTORS, name)) {
                /* Table of function pointers to global/namespace-scope C++
                 * object constructors (see elf_loader_sdk_symbols.c for the
                 * background -- ELF apps are expected to avoid these, but
                 * a dependency can still emit one). Handled exactly like
                 * ".data.rel.ro": copied to a writable RAM buffer so its
                 * R_XTENSA_RELATIVE fixups land somewhere writable, then
                 * walked and invoked once relocation finishes (see the end
                 * of esp_elf_relocate_internal). */
                ESP_LOGD(
                    TAG,
                    ".ctors  sec addr=0x%08x size=0x%08x offset=0x%08x",
                    shdr[i].addr,
                    shdr[i].size,
                    shdr[i].offset
                );

                elf->sec[ELF_SEC_CTORS].v_addr = shdr[i].addr;
                elf->sec[ELF_SEC_CTORS].size = shdr[i].size;
                elf->sec[ELF_SEC_CTORS].offset = shdr[i].offset;

                ESP_LOGD(
                    TAG,
                    ".ctors  offset is 0x%lx size is 0x%x",
                    elf->sec[ELF_SEC_CTORS].offset,
                    elf->sec[ELF_SEC_CTORS].size
                );
            }
        } else if (stype(&shdr[i], SHT_NOBITS) && sflags(&shdr[i], SHF_ALLOC | SHF_WRITE) &&
                   !strcmp(ELF_BSS, name)) {
            ESP_LOGD(
                TAG,
                ".bss    sec addr=0x%08x size=0x%08x offset=0x%08x",
                shdr[i].addr,
                shdr[i].size,
                shdr[i].offset
            );

            elf->sec[ELF_SEC_BSS].v_addr = shdr[i].addr;
            elf->sec[ELF_SEC_BSS].size = shdr[i].size;
            elf->sec[ELF_SEC_BSS].offset = shdr[i].offset;

            ESP_LOGD(
                TAG,
                ".bss    offset is 0x%lx size is 0x%x",
                elf->sec[ELF_SEC_BSS].offset,
                elf->sec[ELF_SEC_BSS].size
            );
        }
    }

    /* No .text on image */

    if (!text_found) { return -EINVAL; }

    elf->sec[ELF_SEC_TEXT].v_addr = text_v_min;
    /* Deliberately NOT ELF_ALIGN()'d: this is also the upper bound
     * esp_elf_map_sym()/esp_elf_map_reloc()/esp_elf_target_streamed() use to
     * decide whether a given vaddr belongs to .text or to whatever section
     * the linker placed immediately after it (usually .rodata, with no
     * padding of its own -- see its "1"-byte alignment in the section
     * header). Rounding this up to a 4-byte boundary used to let it claim up
     * to 3 bytes that actually belong to that next section: a symbol whose
     * vaddr fell in that borrowed gap (e.g. a string literal placed at the
     * very start of .rodata) got mapped through .text's .addr instead of
     * .rodata's, landing on whatever uninitialized/stale bytes happened to
     * sit at the corresponding offset in the .text buffer instead of its
     * real, relocated content -- reproduced by
     * selftest__run_elf_loader_xip_case()'s fixture, whose two-character
     * "OK" literal happens to sit at .rodata's very first byte. Every other
     * consumer that needs a 4-byte-aligned span (xip_rodata_offset, the
     * ptext/streaming allocation sizes) already re-aligns explicitly via its
     * own ELF_ALIGN() call, so this doesn't cost them anything. */
    elf->sec[ELF_SEC_TEXT].size = text_v_max - text_v_min;

    if (!elf->sec[ELF_SEC_TEXT].size) { return -EINVAL; }

    bool xip = elf->xip_ops != NULL;
    elf->xip_rodata_offset = ELF_ALIGN(elf->sec[ELF_SEC_TEXT].size, 4);
    elf->xip_rodata_staged = false;
    if (xip && elf->sec[ELF_SEC_RODATA].size) {
        for (uint32_t i = 0; i < ehdr->shnum; ++i) {
            if (stype(&shdr[i], SHT_RELA) && shdr[i].info == rodata_index) {
                elf->xip_rodata_staged = true;
                break;
            }
        }
    }
    size_t text_work_size = xip && elf->xip_rodata_staged
                                ? elf->xip_rodata_offset + elf->sec[ELF_SEC_RODATA].size
                                : elf->sec[ELF_SEC_TEXT].size;
    size_t xip_size = elf->xip_rodata_offset + elf->sec[ELF_SEC_RODATA].size;

    /* For the XIP/swap target, allocate it FIRST, before attempting the
     * elf->ptext scratch buffer below -- both the normal path and the
     * low-memory streaming fallback (esp_elf_stream_text_to_xip()) need
     * elf->xip_data/xip_handle and sec[ELF_SEC_TEXT/RODATA].addr already
     * set by the time either one runs, and only the fallback skips
     * elf->ptext entirely, so this has to happen before the malloc attempt
     * decides which of the two follows. Doesn't change what the normal path
     * ends up doing, just the order -- allocating the swap target has no
     * dependency on ptext's contents. */
    if (xip) {
        const uint8_t *instruction = NULL;
        const uint8_t *data = NULL;
        int ret = elf->xip_ops->allocate(elf->xip_context, xip_size, &instruction, &data, &elf->xip_handle);
        if (ret != 0 || !instruction || !data || !elf->xip_handle) { return ret != 0 ? ret : -ENOMEM; }
        elf->xip_data = data;
        elf->sec[ELF_SEC_TEXT].addr = (uintptr_t)instruction;
        elf->sec[ELF_SEC_RODATA].addr = (uintptr_t)(data + elf->xip_rodata_offset);
    }

#if CONFIG_BRUCE_QEMU_TEST_MODE
    elf->ptext = (xip && s_debug_force_streaming_fallback) ? NULL : esp_elf_malloc(text_work_size, !xip);
#else
    elf->ptext = esp_elf_malloc(text_work_size, !xip);
#endif
    if (!elf->ptext) {
        if (!xip) {
            ESP_LOGE(
                TAG, "Failed to malloc %" PRIu32 " bytes for text section", (uint32_t)elf->sec[ELF_SEC_TEXT].size
            );
            return -ENOMEM;
        }
        /* No PSRAM, and internal RAM too fragmented for one contiguous
         * text_work_size block -- fall back to streaming the relocated
         * .text/.rodata straight to swap instead of materializing it in RAM
         * first. See esp_elf_stream_text_to_xip()'s doc comment for why
         * this is safe. elf->ptext stays NULL; esp_elf_arch_relocate()
         * treats that (combined with a TEXT/staged-RODATA target) as
         * "already handled here", not an error. */
        ESP_LOGW(
            TAG, "Failed to malloc %" PRIu32 " contiguous bytes for text section, streaming to swap instead",
            (uint32_t)elf->sec[ELF_SEC_TEXT].size
        );
        int ret = esp_elf_stream_text_to_xip(elf, pbuf, ehdr, shdr, text_work_size);
        if (ret != 0) return ret;
    }

    size = elf->sec[ELF_SEC_DATA].size + (xip ? 0 : elf->sec[ELF_SEC_RODATA].size) +
           elf->sec[ELF_SEC_BSS].size + elf->sec[ELF_SEC_DRLRO].size + elf->sec[ELF_SEC_CTORS].size;
    if (size) {
        elf->pdata = esp_elf_malloc(size, false);
        if (!elf->pdata) {
            ESP_LOGE(TAG, "Failed to malloc %" PRIu32 " bytes for data section", size);
            esp_elf_free(elf->ptext);
            elf->ptext = NULL;
            return -ENOMEM;
        }
    }

    if (elf->ptext) {
        /* Dump ".text" (and any ".iram1.*" sections folded into the same
         * region, see above) from ELF to executable space memory. Each
         * section is copied to its own offset within the region
         * individually -- file layout isn't guaranteed to mirror the vaddr
         * layout byte-for-byte (alignment padding between sections), so one
         * bulk memcpy across the whole span isn't safe.
         *
         * Skipped when elf->ptext is NULL: the streaming fallback above
         * already copied (and relocated) this data directly into swap: -- no
         * ptext buffer to fill or point .addr/.reloc_addr at. */
        if (!xip) elf->sec[ELF_SEC_TEXT].addr = (Elf32_Addr)elf->ptext;
        elf->sec[ELF_SEC_TEXT].reloc_addr = (uintptr_t)elf->ptext;
        for (uint32_t i = 0; i < ehdr->shnum; i++) {
            if (stype(&shdr[i], SHT_PROGBITS) && sflags(&shdr[i], SHF_ALLOC) && sflags(&shdr[i], SHF_EXECINSTR)) {
                memcpy(elf->ptext + (shdr[i].addr - text_v_min), pbuf + shdr[i].offset, shdr[i].size);
            }
        }
    }

    if (xip) {
        if (elf->sec[ELF_SEC_RODATA].size && elf->xip_rodata_staged) {
            if (elf->ptext) {
                elf->sec[ELF_SEC_RODATA].reloc_addr = (uintptr_t)(elf->ptext + elf->xip_rodata_offset);
                memcpy(
                    (void *)elf->sec[ELF_SEC_RODATA].reloc_addr,
                    pbuf + elf->sec[ELF_SEC_RODATA].offset,
                    elf->sec[ELF_SEC_RODATA].size
                );
            }
            /* else: the streaming fallback already wrote these bytes -- they're
             * the [text_work_size_text_part, text_work_size) tail of the
             * window loop it ran above. */
        } else if (elf->sec[ELF_SEC_RODATA].size) {
            int ret = elf->xip_ops->write(
                elf->xip_context,
                elf->xip_handle,
                elf->xip_rodata_offset,
                pbuf + elf->sec[ELF_SEC_RODATA].offset,
                elf->sec[ELF_SEC_RODATA].size
            );
            if (ret != 0) return ret;
        }
    }

#ifdef CONFIG_ELF_LOADER_SET_MMU
    if (esp_elf_arch_init_mmu(elf)) {
        esp_elf_free(elf->ptext);
        elf->ptext = NULL;
        esp_elf_free(elf->pdata);
        elf->pdata = NULL;
        return -EIO;
    }
#endif

    /**
     * Dump ".data", ".rodata" and ".bss" from ELF to R/W space memory.
     *
     * Todo: Dump ".rodata" to rodata section by MMU/MPU.
     */

    if (size) {
        uint8_t *pdata = elf->pdata;

        if (elf->sec[ELF_SEC_DATA].size) {
            elf->sec[ELF_SEC_DATA].addr = (uint32_t)pdata;
            elf->sec[ELF_SEC_DATA].reloc_addr = (uintptr_t)pdata;

            memcpy(pdata, pbuf + elf->sec[ELF_SEC_DATA].offset, elf->sec[ELF_SEC_DATA].size);

            pdata += elf->sec[ELF_SEC_DATA].size;
        }

        if (!xip && elf->sec[ELF_SEC_RODATA].size) {
            elf->sec[ELF_SEC_RODATA].addr = (uint32_t)pdata;
            elf->sec[ELF_SEC_RODATA].reloc_addr = (uintptr_t)pdata;

            memcpy(pdata, pbuf + elf->sec[ELF_SEC_RODATA].offset, elf->sec[ELF_SEC_RODATA].size);

            pdata += elf->sec[ELF_SEC_RODATA].size;
        }

        if (elf->sec[ELF_SEC_DRLRO].size) {
            elf->sec[ELF_SEC_DRLRO].addr = (uint32_t)pdata;
            elf->sec[ELF_SEC_DRLRO].reloc_addr = (uintptr_t)pdata;

            memcpy(pdata, pbuf + elf->sec[ELF_SEC_DRLRO].offset, elf->sec[ELF_SEC_DRLRO].size);

            pdata += elf->sec[ELF_SEC_DRLRO].size;
        }

        if (elf->sec[ELF_SEC_CTORS].size) {
            elf->sec[ELF_SEC_CTORS].addr = (uint32_t)pdata;
            elf->sec[ELF_SEC_CTORS].reloc_addr = (uintptr_t)pdata;

            memcpy(pdata, pbuf + elf->sec[ELF_SEC_CTORS].offset, elf->sec[ELF_SEC_CTORS].size);

            pdata += elf->sec[ELF_SEC_CTORS].size;
        }

        if (elf->sec[ELF_SEC_BSS].size) {
            elf->sec[ELF_SEC_BSS].addr = (uint32_t)pdata;
            elf->sec[ELF_SEC_BSS].reloc_addr = (uintptr_t)pdata;
            memset(pdata, 0, elf->sec[ELF_SEC_BSS].size);
        }
    }

    /* Set ELF entry */

    entry = ehdr->entry + elf->sec[ELF_SEC_TEXT].addr - elf->sec[ELF_SEC_TEXT].v_addr;

#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
    elf->entry = (void *)elf_remap_text(elf, (uintptr_t)entry);
#else
    elf->entry = (void *)entry;
#endif

    return 0;
}

#else

/**
 * @brief Load ELF segment.
 *
 * @param elf - ELF object pointer
 * @param pbuf - ELF data buffer
 *
 * @return ESP_OK if success or other if failed.
 */
static int esp_elf_load_segment(esp_elf_t *elf, const uint8_t *pbuf) {
    uint32_t size;
    bool first_segment = false;
    Elf32_Addr vaddr_s = 0;
    Elf32_Addr vaddr_e = 0;

    const elf32_hdr_t *ehdr = (const elf32_hdr_t *)pbuf;
    const elf32_phdr_t *phdr = (const elf32_phdr_t *)(pbuf + ehdr->phoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != PT_LOAD) { continue; }

        if (phdr[i].memsz < phdr[i].filesz) {
            ESP_LOGE(TAG, "Invalid segment[%d], memsz: %d, filesz: %d", i, phdr[i].memsz, phdr[i].filesz);
            return -EINVAL;
        }

        if (first_segment == true) {
            vaddr_s = phdr[i].vaddr;
            vaddr_e = phdr[i].vaddr + phdr[i].memsz;
            first_segment = true;
            if (vaddr_e < vaddr_s) {
                ESP_LOGE(TAG, "Invalid segment[%d], vaddr: 0x%x, memsz: %d", i, phdr[i].vaddr, phdr[i].memsz);
                return -EINVAL;
            }
        } else {
            if (phdr[i].vaddr < vaddr_e) {
                ESP_LOGE(
                    TAG,
                    "Invalid segment[%d], should not overlap, vaddr: 0x%x, vaddr_e: 0x%x",
                    i,
                    phdr[i].vaddr,
                    vaddr_e
                );
                return -EINVAL;
            }

            if (phdr[i].vaddr > vaddr_e + ADDR_OFFSET) {
                ESP_LOGI(TAG, "Too much padding before segment[%d], padding: %d", i, phdr[i].vaddr - vaddr_e);
            }

            vaddr_e = phdr[i].vaddr + phdr[i].memsz;
            if (vaddr_e < phdr[i].vaddr) {
                ESP_LOGE(
                    TAG,
                    "Invalid segment[%d], address overflow, vaddr: 0x%x, vaddr_e: 0x%x",
                    i,
                    phdr[i].vaddr,
                    vaddr_e
                );
                return -EINVAL;
            }
        }

        ESP_LOGD(TAG, "LOAD segment[%d], vaddr: 0x%x, memsize: 0x%08x", i, phdr[i].vaddr, phdr[i].memsz);
    }

    size = vaddr_e - vaddr_s;
    if (size == 0) { return -EINVAL; }

    elf->svaddr = vaddr_s;
    elf->psegment = esp_elf_malloc(size, true);
    if (!elf->psegment) { return -ENOMEM; }

    memset(elf->psegment, 0, size);

    /* Dump "PT_LOAD" from ELF to memory space */

    for (int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type == PT_LOAD) {
            memcpy(elf->psegment + phdr[i].vaddr - vaddr_s, (uint8_t *)pbuf + phdr[i].offset, phdr[i].filesz);
            ESP_LOGD(
                TAG,
                "Copy segment[%d], mem_addr: %p, size: 0x%08x",
                i,
                (void *)((uint8_t *)elf->psegment + phdr[i].vaddr - vaddr_s),
                phdr[i].filesz
            );
        }
    }

#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
    cache_ll_writeback_all(CACHE_LL_LEVEL_INT_MEM, CACHE_TYPE_DATA, CACHE_LL_ID_ALL);
#endif

    elf->entry = (void *)((uint8_t *)elf->psegment + ehdr->entry - vaddr_s);

    return 0;
}
#endif

/**
 * @brief Override the internal symbol resolver.
 * The default resolver is based on static lists that are determined by KConfig.
 * This override allows for an arbitrary implementation.
 *
 * @param resolver the resolver function
 */
void elf_set_symbol_resolver(symbol_resolver resolver) {
    if (!resolver) {
        ESP_LOGE(TAG, "Invalid resolver: cannot set NULL resolver");
        return;
    }

    atomic_store(&current_resolver, resolver);
}

/**
 * @brief Reset the symbol resolver to the default (static tables from KConfig).
 *
 * Equivalent to elf_set_symbol_resolver(elf_find_sym_default).
 */
void elf_reset_symbol_resolver(void) { atomic_store(&current_resolver, elf_find_sym_default); }

/**
 * @brief Map symbol's address of ELF to physic space.
 *
 * @param elf - ELF object pointer
 * @param sym - ELF symbol address
 *
 * @return ESP_OK if success or other if failed.
 */
uintptr_t esp_elf_map_sym(esp_elf_t *elf, uintptr_t sym) {
    for (int i = 0; i < ELF_SECS; i++) {
        if ((sym >= elf->sec[i].v_addr) && (sym < (elf->sec[i].v_addr + elf->sec[i].size))) {
            return sym - elf->sec[i].v_addr + elf->sec[i].addr;
        }
    }

    return 0;
}

uintptr_t esp_elf_map_reloc(esp_elf_t *elf, uintptr_t sym) {
    for (int i = 0; i < ELF_SECS; i++) {
        if ((sym >= elf->sec[i].v_addr) && (sym < (elf->sec[i].v_addr + elf->sec[i].size)) &&
            elf->sec[i].reloc_addr) {
            return sym - elf->sec[i].v_addr + elf->sec[i].reloc_addr;
        }
    }
    return 0;
}

/**
 * @brief Initialize ELF object.
 *
 * @param elf - ELF object pointer
 *
 * @return ESP_OK if success or other if failed.
 */
int esp_elf_init(esp_elf_t *elf) {
    ESP_LOGI(
        TAG, "ELF loader version: %d.%d.%d", ELF_LOADER_VER_MAJOR, ELF_LOADER_VER_MINOR, ELF_LOADER_VER_PATCH
    );

    if (!elf) { return -EINVAL; }

    memset(elf, 0, sizeof(esp_elf_t));

    return 0;
}

/**
 * @brief Invoke global/namespace-scope C++ object constructors registered in
 *        ".ctors" (see esp_elf_load_section() and elf_loader_sdk_symbols.c
 *        for how that table gets there and why).
 *
 * Every entry has already been fixed up to its final, callable runtime
 * address by the ordinary R_XTENSA_RELATIVE relocation pass in
 * esp_elf_relocate_internal() -- the same pass and the same section-address
 * mapping that produces elf->entry -- so this just walks the table and
 * calls each one before the caller runs the app's own entry point.
 *
 * Legacy ".ctors" tables are conventionally terminated by a -1 sentinel
 * when built via crtbegin.o/crtend.o; project_elf() links with -nostdlib so
 * neither object is linked in and no sentinel is present here, but 0/-1
 * entries are skipped defensively in case that ever changes.
 *
 * @param elf - ELF object pointer
 */
static void esp_elf_run_ctors(esp_elf_t *elf) {
    size_t count = elf->sec[ELF_SEC_CTORS].size / sizeof(uint32_t);
    if (!count) { return; }

    const uint32_t *ctors = (const uint32_t *)elf->sec[ELF_SEC_CTORS].reloc_addr;

    for (size_t i = 0; i < count; i++) {
        uint32_t ctor = ctors[i];
        if (ctor == 0 || ctor == UINT32_MAX) { continue; }

        ESP_LOGD(TAG, "Calling global constructor[%u] at 0x%08x", (unsigned)i, ctor);
        ((void (*)(void))ctor)();
    }
}

/**
 * @brief Decode and relocate ELF data.
 *
 * @param elf - ELF object pointer
 * @param pbuf - ELF data buffer
 *
 * @return ESP_OK if success or other if failed.
 */
static int esp_elf_relocate_internal(esp_elf_t *elf, const uint8_t *pbuf) {
    int ret;

    const elf32_hdr_t *ehdr;
    const elf32_shdr_t *shdr;
    const char *shstrab;
    const elf32_sym_t *symtab;
    const char *strtab;

    if (!elf || !pbuf) { return -EINVAL; }

    ehdr = (const elf32_hdr_t *)pbuf;
    shdr = (const elf32_shdr_t *)(pbuf + ehdr->shoff);
    shstrab = (const char *)pbuf + shdr[ehdr->shstrndx].offset;

    /* Load section or segment to memory space */

#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
    ret = esp_elf_load_section(elf, pbuf);
#else
    ret = esp_elf_load_segment(elf, pbuf);
#endif

    if (ret) {
        ESP_LOGE(TAG, "Error to load elf file, ret=%d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "elf->entry=%p", elf->entry);

    /* Relocation section data */

    for (uint32_t i = 0; i < ehdr->shnum; i++) {
        if (stype(&shdr[i], SHT_RELA)) {
            uint32_t nr_reloc;
            const elf32_rela_t *rela;

            nr_reloc = shdr[i].size / sizeof(elf32_rela_t);
            rela = (const elf32_rela_t *)(pbuf + shdr[i].offset);
            symtab = (const elf32_sym_t *)(pbuf + shdr[shdr[i].link].offset);
            strtab = (const char *)(pbuf + shdr[shdr[shdr[i].link].link].offset);

            ESP_LOGD(TAG, "Section %s has %d symbol tables", shstrab + shdr[i].name, (int)nr_reloc);

            for (int i = 0; i < nr_reloc; i++) {
                uintptr_t addr = 0;
                elf32_rela_t rela_buf;

                memcpy(&rela_buf, &rela[i], sizeof(elf32_rela_t));

                const elf32_sym_t *sym = &symtab[ELF_R_SYM(rela_buf.info)];

                ret = esp_elf_resolve_relocation_addr(elf, &rela_buf, sym, strtab, &addr);
                if (ret != 0) { return ret; }

                ret = esp_elf_arch_relocate(elf, &rela_buf, sym, addr);
                if (ret != 0) { return ret; }
            }
#if CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
        } else {
            if (strcmp((const char *)(shstrab + shdr[i].name), ELF_DYNSYM) == 0) {
                int j;
                uint32_t len;
                uint16_t num = 0;
                elf->num = 0;
                symtab = (const elf32_sym_t *)(pbuf + shdr[i].offset);
                strtab = (const char *)(pbuf + shdr[shdr[i].link].offset);
                for (j = 0; j < shdr[i].size / sizeof(elf32_sym_t); j++) {
                    if ((ELF_ST_BIND(symtab[j].info) == STB_GLOBAL) &&
                        (ELF_ST_TYPE(symtab[j].info) == STT_FUNC)) {
                        elf->num++;
                    }
                }

                if (elf->num) {
                    elf->symtab = (esp_symtab_t *)esp_elf_malloc(elf->num * sizeof(esp_symtab_t), false);
                    if (!elf->symtab) {
                        ESP_LOGE(TAG, "Failed to malloc for symbol table");
                        return -ENOMEM;
                    }

                    memset(elf->symtab, 0, elf->num * sizeof(esp_symtab_t));
                }

                for (j = 0; j < shdr[i].size / sizeof(elf32_sym_t); j++) {
                    if ((ELF_ST_BIND(symtab[j].info) == STB_GLOBAL) &&
                        (ELF_ST_TYPE(symtab[j].info) == STT_FUNC)) {
                        len = strlen((const char *)(strtab + symtab[j].name)) + 1;
#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
                        elf->symtab[num].addr =
                            (void *)(elf->ptext + symtab[j].value - elf->sec[ELF_SEC_TEXT].v_addr);
#else
                        elf->symtab[num].addr = (void *)(elf->psegment + symtab[j].value - elf->svaddr);
#endif
                        elf->symtab[num].name = esp_elf_malloc(len, false);
                        if (!elf->symtab[num].name) {
                            ESP_LOGE(TAG, "Failed to malloc for symbol table name");
                            elf->num = num;
                            return -ENOMEM;
                        }

                        memset((void *)elf->symtab[num].name, 0, len);
                        memcpy((void *)elf->symtab[num].name, strtab + symtab[j].name, len);
                        ESP_LOGI(TAG, "elf->symtab[%d], func: %s", num, strtab + symtab[j].name);
                        num++;
                    }
                }
            }
#endif
        }
    }

#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
    esp_elf_arch_flush();
#endif

    /* elf->ptext == NULL here means every relocated byte was already streamed
     * straight to the xip target during esp_elf_load_section() (the
     * streaming-fallback path, see esp_elf_arch_relocate()'s doc comment in
     * esp_elf_xtensa.c) -- there is nothing left in RAM to write or verify,
     * and elf->ptext is not a valid pointer to pass to xip_ops->write()/
     * memcmp() below. */
    if (elf->xip_ops && elf->ptext != NULL) {
        ret = elf->xip_ops->write(
            elf->xip_context, elf->xip_handle, 0, elf->ptext, elf->sec[ELF_SEC_TEXT].size
        );
        if (ret == 0 && elf->sec[ELF_SEC_RODATA].size && elf->xip_rodata_staged) {
            ret = elf->xip_ops->write(
                elf->xip_context,
                elf->xip_handle,
                elf->xip_rodata_offset,
                (const void *)elf->sec[ELF_SEC_RODATA].reloc_addr,
                elf->sec[ELF_SEC_RODATA].size
            );
        }
        if (ret != 0) return ret;
        if (memcmp(elf->xip_data, elf->ptext, elf->sec[ELF_SEC_TEXT].size) != 0 ||
            (elf->sec[ELF_SEC_RODATA].size && elf->xip_rodata_staged &&
             memcmp(
                 elf->xip_data + elf->xip_rodata_offset,
                 (const void *)elf->sec[ELF_SEC_RODATA].reloc_addr,
                 elf->sec[ELF_SEC_RODATA].size
             ) != 0)) {
            return -EIO;
        }
        esp_elf_free(elf->ptext);
        elf->ptext = NULL;
    }

    esp_elf_run_ctors(elf);

    return 0;
}

int esp_elf_relocate(esp_elf_t *elf, const uint8_t *pbuf) { return esp_elf_relocate_internal(elf, pbuf); }

int esp_elf_relocate_xip(
    esp_elf_t *elf, const uint8_t *pbuf, size_t size, const esp_elf_xip_ops_t *ops, void *context
) {
#if !CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
    (void)elf;
    (void)pbuf;
    (void)size;
    (void)ops;
    (void)context;
    return -ENOTSUP;
#else
    if (!elf || !pbuf || !ops || !ops->allocate || !ops->write || !ops->release ||
        size < sizeof(elf32_hdr_t)) {
        return -EINVAL;
    }
    const elf32_hdr_t *ehdr = (const elf32_hdr_t *)pbuf;
    if (memcmp(
            ehdr->ident,
            "\x7f"
            "ELF",
            4
        ) != 0 ||
        ehdr->ident[4] != 1 || ehdr->ident[5] != 1 || ehdr->machine != 94 ||
        ehdr->shentsize != sizeof(elf32_shdr_t) || ehdr->shnum == 0 || ehdr->shstrndx >= ehdr->shnum ||
        ehdr->shoff > size || (size_t)ehdr->shnum > (size - ehdr->shoff) / sizeof(elf32_shdr_t)) {
        return -EINVAL;
    }
    const elf32_shdr_t *shdr = (const elf32_shdr_t *)(pbuf + ehdr->shoff);
    for (uint32_t i = 0; i < ehdr->shnum; ++i) {
        if (shdr[i].type != SHT_NOBITS && (shdr[i].offset > size || shdr[i].size > size - shdr[i].offset)) {
            return -EINVAL;
        }
    }
    const elf32_shdr_t *shstr = &shdr[ehdr->shstrndx];
    if (shstr->offset > size || shstr->size > size - shstr->offset) {
        return -EINVAL;
    }
    const char *section_names = (const char *)pbuf + shstr->offset;
    for (uint32_t i = 0; i < ehdr->shnum; ++i) {
        if (shdr[i].name >= shstr->size ||
            memchr(section_names + shdr[i].name, '\0', shstr->size - shdr[i].name) == NULL) {
            return -EINVAL;
        }
        if (shdr[i].type != SHT_RELA) continue;
        if (shdr[i].size % sizeof(elf32_rela_t) != 0 || shdr[i].link >= ehdr->shnum) {
            return -EINVAL;
        }
        const elf32_shdr_t *symbols = &shdr[shdr[i].link];
        if (symbols->size % sizeof(elf32_sym_t) != 0 || symbols->link >= ehdr->shnum) {
            return -EINVAL;
        }
        const elf32_shdr_t *strings = &shdr[symbols->link];
        size_t symbol_count = symbols->size / sizeof(elf32_sym_t);
        const elf32_rela_t *relocations = (const elf32_rela_t *)(pbuf + shdr[i].offset);
        const elf32_sym_t *symbol_table = (const elf32_sym_t *)(pbuf + symbols->offset);
        const char *string_table = (const char *)pbuf + strings->offset;
        for (size_t j = 0; j < shdr[i].size / sizeof(elf32_rela_t); ++j) {
            size_t symbol_index = ELF_R_SYM(relocations[j].info);
            if (symbol_index >= symbol_count || symbol_table[symbol_index].name >= strings->size ||
                memchr(
                    string_table + symbol_table[symbol_index].name,
                    '\0',
                    strings->size - symbol_table[symbol_index].name
                ) == NULL) {
                return -EINVAL;
            }
        }
    }
    elf->xip_ops = ops;
    elf->xip_context = context;
    int ret = esp_elf_relocate_internal(elf, pbuf);
    if (ret != 0) esp_elf_deinit(elf);
    return ret;
#endif
}

/**
 * @brief Request running relocated ELF function.
 *
 * @param elf  - ELF object pointer
 * @param opt  - Request options
 * @param argc - Arguments number
 * @param argv - Arguments value array
 *
 * @return ESP_OK if success or other if failed.
 */
int esp_elf_request(esp_elf_t *elf, int opt, int argc, char *argv[]) {
    if (!elf || !(elf->entry)) { return -EINVAL; }

    elf->entry(argc, argv);

    return 0;
}

/**
 * @brief Deinitialize ELF object.
 *
 * @param elf - ELF object pointer
 *
 * @return None
 *
 * @note This function frees all resources allocated by esp_elf_relocate() and
 *       resets the structure to its initial state (same as after esp_elf_init()).
 */
void esp_elf_deinit(esp_elf_t *elf) {
    if (!elf) { return; }

    if (elf->xip_handle && elf->xip_ops && elf->xip_ops->release) {
        elf->xip_ops->release(elf->xip_context, elf->xip_handle);
        elf->xip_handle = 0;
    }

#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
    if (elf->pdata) {
        esp_elf_free(elf->pdata);
        elf->pdata = NULL;
    }

    if (elf->ptext) {
        esp_elf_free(elf->ptext);
        elf->ptext = NULL;
    }
#else
    if (elf->psegment) {
        esp_elf_free(elf->psegment);
        elf->psegment = NULL;
    }
#endif

#ifdef CONFIG_ELF_LOADER_SET_MMU
    esp_elf_arch_deinit_mmu(elf);
#endif

#if CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
    if (elf->num && elf->symtab) {
        for (int i = 0; i < elf->num; i++) {
            if (elf->symtab[i].name) { esp_elf_free(elf->symtab[i].name); }
        }

        esp_elf_free(elf->symtab);
        elf->symtab = NULL;
    }

    elf->num = 0;
#endif

    /* Reset structure to initial state (same as esp_elf_init) */
    memset(elf, 0, sizeof(esp_elf_t));
}

/**
 * @brief Print header description information of ELF.
 *
 * @param pbuf - ELF data buffer
 *
 * @return None
 */
void esp_elf_print_ehdr(const uint8_t *pbuf) {
    const char *s_bits, *s_endian;
    const elf32_hdr_t *hdr = (const elf32_hdr_t *)pbuf;

    switch (hdr->ident[4]) {
        case 1: s_bits = "32-bit"; break;
        case 2: s_bits = "64-bit"; break;
        default: s_bits = "invalid bits"; break;
    }

    switch (hdr->ident[5]) {
        case 1: s_endian = "little-endian"; break;
        case 2: s_endian = "big-endian"; break;
        default: s_endian = "invalid endian"; break;
    }

    if (hdr->ident[0] == 0x7f) {
        ESP_LOGI(TAG, "%-40s %c%c%c", "Class:", hdr->ident[1], hdr->ident[2], hdr->ident[3]);
    }

    ESP_LOGI(TAG, "%-40s %s, %s", "Format:", s_bits, s_endian);
    ESP_LOGI(TAG, "%-40s %x", "Version(current):", hdr->ident[6]);

    ESP_LOGI(TAG, "%-40s %d", "Type:", hdr->type);
    ESP_LOGI(TAG, "%-40s %d", "Machine:", hdr->machine);
    ESP_LOGI(TAG, "%-40s %x", "Version:", hdr->version);
    ESP_LOGI(TAG, "%-40s %x", "Entry point address:", hdr->entry);
    ESP_LOGI(TAG, "%-40s %x", "Start of program headers:", hdr->phoff);
    ESP_LOGI(TAG, "%-40s %d", "Start of section headers:", hdr->shoff);
    ESP_LOGI(TAG, "%-40s 0x%x", "Flags:", hdr->flags);
    ESP_LOGI(TAG, "%-40s %d", "Size of this header(bytes):", hdr->ehsize);
    ESP_LOGI(TAG, "%-40s %d", "Size of program headers(bytes):", hdr->phentsize);
    ESP_LOGI(TAG, "%-40s %d", "Number of program headers:", hdr->phnum);
    ESP_LOGI(TAG, "%-40s %d", "Size of section headers(bytes):", hdr->shentsize);
    ESP_LOGI(TAG, "%-40s %d", "Number of section headers:", hdr->shnum);
    ESP_LOGI(TAG, "%-40s %d", "Section header string table i:", hdr->shstrndx);
}

/**
 * @brief Print program header description information of ELF.
 *
 * @param pbuf - ELF data buffer
 *
 * @return None
 */
void esp_elf_print_phdr(const uint8_t *pbuf) {
    const elf32_hdr_t *ehdr = (const elf32_hdr_t *)pbuf;
    const elf32_phdr_t *phdr = (const elf32_phdr_t *)((size_t)pbuf + ehdr->phoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        ESP_LOGI(TAG, "%-40s %d", "type:", phdr->type);
        ESP_LOGI(TAG, "%-40s 0x%x", "offset:", phdr->offset);
        ESP_LOGI(TAG, "%-40s 0x%x", "vaddr", phdr->vaddr);
        ESP_LOGI(TAG, "%-40s 0x%x", "paddr:", phdr->paddr);
        ESP_LOGI(TAG, "%-40s %d", "filesz", phdr->filesz);
        ESP_LOGI(TAG, "%-40s %d", "memsz", phdr->memsz);
        ESP_LOGI(TAG, "%-40s %d", "flags", phdr->flags);
        ESP_LOGI(TAG, "%-40s 0x%x", "align", phdr->align);

        phdr = (const elf32_phdr_t *)((size_t)phdr + sizeof(elf32_phdr_t));
    }
}

/**
 * @brief Print section header description information of ELF.
 *
 * @param pbuf - ELF data buffer
 *
 * @return None
 */
void esp_elf_print_shdr(const uint8_t *pbuf) {
    const elf32_hdr_t *ehdr = (const elf32_hdr_t *)pbuf;
    const elf32_shdr_t *shdr = (const elf32_shdr_t *)((size_t)pbuf + ehdr->shoff);

    for (int i = 0; i < ehdr->shnum; i++) {
        ESP_LOGI(TAG, "%-40s %d", "name:", shdr->name);
        ESP_LOGI(TAG, "%-40s %d", "type:", shdr->type);
        ESP_LOGI(TAG, "%-40s 0x%x", "flags:", shdr->flags);
        ESP_LOGI(TAG, "%-40s %x", "addr", shdr->addr);
        ESP_LOGI(TAG, "%-40s %x", "offset:", shdr->offset);
        ESP_LOGI(TAG, "%-40s %d", "size", shdr->size);
        ESP_LOGI(TAG, "%-40s 0x%x", "link", shdr->link);
        ESP_LOGI(TAG, "%-40s %d", "addralign", shdr->addralign);
        ESP_LOGI(TAG, "%-40s %d", "entsize", shdr->entsize);

        shdr = (const elf32_shdr_t *)((size_t)shdr + sizeof(elf32_shdr_t));
    }
}

/**
 * @brief Print section information of ELF.
 *
 * @param pbuf - ELF data buffer
 *
 * @return None
 */
void esp_elf_print_sec(esp_elf_t *elf) {
    const char *sec_names[ELF_SECS] = {"text", "bss", "data", "rodata"};

    for (int i = 0; i < ELF_SECS; i++) {
        ESP_LOGI(TAG, "%s:   0x%08x size 0x%08x", sec_names[i], elf->sec[i].addr, elf->sec[i].size);
    }

    ESP_LOGI(TAG, "entry:  %p", elf->entry);
}

/**
 * @brief Register symbol table to global symbol tables array.
 *
 * @param symbol_table - Pointer to symbol table structure (array of esp_elfsym terminated by ESP_ELFSYM_END)
 *
 * @return 0 if success, -EINVAL if symbol_table is NULL, -EEXIST if already registered, -ENOMEM if no space.
 *
 * @note This function is not thread-safe. External synchronization must be used if calling
 *       this function concurrently from multiple threads.
 */
int esp_elf_register_symbol(esp_elf_symbol_table_t *symbol_table) {
    if (!symbol_table) { return -EINVAL; }

    for (int i = 0; i < SYMBOL_TABLES_NO; i++) {
        if (g_symbol_tables[i] == symbol_table) {
            return -EEXIST;
        } else if (g_symbol_tables[i] == NULL) {
            g_symbol_tables[i] = symbol_table;
            return 0;
        }
    }

    return -ENOMEM;
}

/**
 * @brief Unregister symbol table from global symbol tables array.
 *
 * @param symbol_table - Pointer to symbol table structure to remove
 *
 * @return 0 if success, -EINVAL if symbol_table is NULL or symbol table not found.
 *
 * @note This function is not thread-safe. External synchronization must be used if calling
 *       this function concurrently from multiple threads.
 */
int esp_elf_unregister_symbol(esp_elf_symbol_table_t *symbol_table) {
    if (!symbol_table) { return -EINVAL; }

    for (int i = 0; i < SYMBOL_TABLES_NO; i++) {
        if (g_symbol_tables[i] == symbol_table) {
            g_symbol_tables[i] = NULL;
            return 0;
        }
    }

    return -EINVAL;
}

/**
 * @brief Find symbol address by symbol name in registered tables.
 *
 * @param sym_name - Symbol name string to search
 *
 * @return Symbol address if found, 0 if not found.
 * @note Search order is registration order (earliest registered first).
 */
uintptr_t esp_elf_find_symbol(const char *sym_name) {
    if (!sym_name) { return 0; }

    esp_elf_symbol_table_t *syms;
    for (int i = 0; i < SYMBOL_TABLES_NO; i++) {
        if (g_symbol_tables[i]) {
            syms = g_symbol_tables[i];
            while (syms->name) {
                if (!strcmp(syms->name, sym_name)) { return (uintptr_t)syms->sym; }

                syms++;
            }
        }
    }

    return 0;
}
