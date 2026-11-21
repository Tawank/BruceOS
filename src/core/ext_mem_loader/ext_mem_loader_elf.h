#pragma once

#include "core_sdk/ext_mem_loader.h"
#include "esp_elf.h"

/* esp_elf_relocate_xip() callbacks that place an ELF's code and read-only
 * data in swap. The context is the bruce_ext_mem_loader_xip_image_t that
 * receives the mapping. Used by the ELF app loader and core/lib. */
extern const esp_elf_xip_ops_t ext_mem_loader__elf_xip_ops;
