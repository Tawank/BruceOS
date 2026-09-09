/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "private/elf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Notes: align_size needs to be a power of 2 */

#define ELF_ALIGN(_a, align_size) (((_a) + (align_size - 1)) & \
                                   ~(align_size - 1))

/**
 * @brief Allocate block of memory.
 *
 * @param n - Memory size in byte
 * @param exec - True: memory can run executable code; False: memory can R/W data
 *
 * @return Memory pointer if success or NULL if failed.
 */
void *esp_elf_malloc(uint32_t n, bool exec);

/**
 * @brief Free block of memory.
 *
 * @param ptr - memory block pointer allocated by "esp_elf_malloc"
 *
 * @return None
 */
void esp_elf_free(void *ptr);

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
                          const elf32_sym_t *sym, uint32_t addr);

/**
 * @brief Applies one already-resolved relocation through a caller-supplied
 * target pointer instead of looking one up via esp_elf_map_reloc() --
 * esp_elf_arch_relocate() factored apart so esp_elf.c's low-memory
 * streaming fallback (esp_elf_stream_text_to_xip()) can reuse the exact
 * same per-type relocation semantics against its own window buffer. See
 * esp_elf_xtensa.c's definition for the full rationale.
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
                             const elf32_sym_t *sym, uint32_t addr, uint32_t *where);

/**
 * @brief Checks whether an ELF virtual address falls within the region
 * esp_elf_stream_text_to_xip() streams straight to swap (see its doc
 * comment in esp_elf.c) instead of a materialized elf->ptext buffer -- the
 * low-memory fallback esp_elf_load_section() takes when esp_elf_malloc()
 * fails for ptext but a swap/XIP target is available. Used by
 * esp_elf_arch_relocate() to tell "this relocation's target was already
 * handled by that fallback" apart from a genuine unmappable-target error,
 * and internally by the streaming function itself to place each relocation
 * within its current window.
 *
 * @param elf        - ELF object pointer
 * @param offset     - ELF virtual address to check (a relocation's rela->offset)
 * @param out_target - If non-NULL and this returns true, set to the
 *                      corresponding byte offset within the streamed region
 *                      (.text bytes first, then, if staged, .rodata bytes
 *                      starting at the 4-byte-aligned elf->xip_rodata_offset
 *                      -- the same layout esp_elf_stream_text_to_xip()
 *                      writes to swap in)
 *
 * @return true if `offset` falls within the streamed .text/.rodata region
 */
bool esp_elf_target_streamed(esp_elf_t *elf, uintptr_t offset, size_t *out_target);

/**
 * @brief Remap symbol from ".data" to ".text" section.
 *
 * @param elf  - ELF object pointer
 * @param sym  - ELF symbol table
 *
 * @return Remapped symbol value
 */
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
uintptr_t elf_remap_text(esp_elf_t *elf, uintptr_t sym);
#endif

/**
 * @brief Flush data from cache to external RAM.
 *
 * @param None
 *
 * @return None
 */
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
void esp_elf_arch_flush(void);
#endif

/**
 * @brief Initialize MMU hardware remapping function.
 *
 * @param elf - ELF object pointer
 *
 * @return 0 if success or a negative value if failed.
 */
#ifdef CONFIG_ELF_LOADER_SET_MMU
int esp_elf_arch_init_mmu(esp_elf_t *elf);
#endif

/**
 * @brief De-initialize MMU hardware remapping function.
 *
 * @param elf - ELF object pointer
 *
 * @return None
 */
#ifdef CONFIG_ELF_LOADER_SET_MMU
void esp_elf_arch_deinit_mmu(esp_elf_t *elf);
#endif

#ifdef __cplusplus
}
#endif
