/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <sys/errno.h>
#include "esp_elf.h"
#include "esp_log.h"
#include "private/elf_platform.h"

/** @brief Xtensa relocations defined by the ABIs */

#define R_XTENSA_NONE           0
#define R_XTENSA_32             1
#define R_XTENSA_RTLD           2
#define R_XTENSA_GLOB_DAT       3
#define R_XTENSA_JMP_SLOT       4
#define R_XTENSA_RELATIVE       5
#define R_XTENSA_PLT            6
#define R_XTENSA_OP0            8
#define R_XTENSA_OP1            9
#define R_XTENSA_OP2            10
#define R_XTENSA_ASM_EXPAND     11
#define R_XTENSA_ASM_SIMPLIFY   12
#define R_XTENSA_GNU_VTINHERIT  15
#define R_XTENSA_GNU_VTENTRY    16
#define R_XTENSA_DIFF8          17
#define R_XTENSA_DIFF16         18
#define R_XTENSA_DIFF32         19
#define R_XTENSA_SLOT0_OP       20
#define R_XTENSA_SLOT1_OP       21
#define R_XTENSA_SLOT2_OP       22
#define R_XTENSA_SLOT3_OP       23
#define R_XTENSA_SLOT4_OP       24
#define R_XTENSA_SLOT5_OP       25
#define R_XTENSA_SLOT6_OP       26
#define R_XTENSA_SLOT7_OP       27
#define R_XTENSA_SLOT8_OP       28
#define R_XTENSA_SLOT9_OP       29
#define R_XTENSA_SLOT10_OP      30
#define R_XTENSA_SLOT11_OP      31
#define R_XTENSA_SLOT12_OP      32
#define R_XTENSA_SLOT13_OP      33
#define R_XTENSA_SLOT14_OP      34
#define R_XTENSA_SLOT0_ALT      35
#define R_XTENSA_SLOT1_ALT      36
#define R_XTENSA_SLOT2_ALT      37
#define R_XTENSA_SLOT3_ALT      38
#define R_XTENSA_SLOT4_ALT      39
#define R_XTENSA_SLOT5_ALT      40
#define R_XTENSA_SLOT6_ALT      41
#define R_XTENSA_SLOT7_ALT      42
#define R_XTENSA_SLOT8_ALT      43
#define R_XTENSA_SLOT9_ALT      44
#define R_XTENSA_SLOT10_ALT     45
#define R_XTENSA_SLOT11_ALT     46
#define R_XTENSA_SLOT12_ALT     47
#define R_XTENSA_SLOT13_ALT     48
#define R_XTENSA_SLOT14_ALT     49

static const char *TAG = "elf_arch";

/**
 * @brief Applies one already-resolved relocation directly through a caller-
 * supplied target pointer, instead of looking one up via
 * esp_elf_map_reloc(). Factored out of esp_elf_arch_relocate() so the
 * low-memory streaming fallback in esp_elf.c (esp_elf_stream_text_to_xip())
 * can apply the exact same per-type relocation semantics against its own
 * small window buffer -- there is nothing in this switch that depends on
 * `where` pointing into elf->ptext specifically, only that it's 4 writable
 * (and, for R_XTENSA_RELATIVE, readable) bytes holding this relocation's
 * pre-relocation placeholder.
 *
 * @param elf   - ELF object pointer
 * @param rela  - Relocated symbol data
 * @param sym   - ELF symbol table
 * @param addr  - Jumping target address
 * @param where - Writable pointer to the relocation's target 4 bytes
 *
 * @return ESP_OK if success or other if failed.
 */
int esp_elf_arch_relocate_at(esp_elf_t *elf, const elf32_rela_t *rela,
                             const elf32_sym_t *sym, uint32_t addr, uint32_t *where)
{
    uint32_t val;

    assert(elf && rela && where);

    ESP_LOGD(TAG, "type: %d, where=%p addr=0x%x offset=0x%x\n",
             ELF_R_TYPE(rela->info), where, (int)addr, (int)rela->offset);

    switch (ELF_R_TYPE(rela->info)) {
    case R_XTENSA_RELATIVE:
        val = esp_elf_map_sym(elf, *where);
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
        *where = elf_remap_text(elf, val);
#else
        *where  = val;
#endif
        break;
    case R_XTENSA_RTLD:
        break;
    case R_XTENSA_GLOB_DAT:
    case R_XTENSA_JMP_SLOT:
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
        *where = elf_remap_text(elf, addr);
#else
        *where = addr;
#endif
        break;
    default:
        ESP_LOGE(TAG, "info=%d is not supported", ELF_R_TYPE(rela->info));
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief Relocates target architecture symbol of ELF
 *
 * @param elf  - ELF object pointer
 * @param rela - Relocated symbol data
 * @param sym  - ELF symbol table
 * @param addr - Jumping target address
 *
 * @return ESP_OK if success or other if failed.
 */
int esp_elf_arch_relocate(esp_elf_t *elf, const elf32_rela_t *rela,
                          const elf32_sym_t *sym, uint32_t addr)
{
    uint32_t *where;

    assert(elf && rela);

    where = (uint32_t *)esp_elf_map_reloc(elf, rela->offset);
    if (!where) {
        /* elf->ptext == NULL here normally means "out of memory, this
         * relocation target is unmappable" -- except when this ELF's .text
         * (and any staged .rodata) went through esp_elf_stream_text_to_xip()
         * instead of a materialized ptext buffer (the low-RAM fallback
         * esp_elf.c's esp_elf_load_section() takes when esp_elf_malloc()
         * for ptext fails but a swap/XIP target is available). That
         * function already applies every relocation
         * landing in its region itself, window by window -- reaching here
         * for one of those offsets means it's already handled, not broken. */
        if (elf->xip_ops != NULL && elf->ptext == NULL &&
            esp_elf_target_streamed(elf, rela->offset, NULL)) {
            return 0;
        }
        ESP_LOGE(TAG, "relocation target 0x%x is not writable", (int)rela->offset);
        return -EINVAL;
    }

    return esp_elf_arch_relocate_at(elf, rela, sym, addr, where);
}
