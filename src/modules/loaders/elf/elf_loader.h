#pragma once

#include <stddef.h>

/*
 * Built-in ELF loader module (see migration_BruceIDF.md, "Loader modules" /
 * "ELF contract").  This is an ordinary built-in module - it includes only
 * core_sdk/... headers and gets no private-Core-header exemption, unlike
 * modules/selftest.
 */

/* Registers this module: a built-in "elf" command that loads an arbitrary
 * ELF file by path, plus the ".elf" loader at priority 10.  Called once from
 * app_runner__register_defaults(). */
void elf_loader__register(void);

/* Test-only observability for modules/selftest (exempt from the
 * core_sdk-only-headers rule); not part of any public contract.  Counts
 * completed calls into elf_loader__run_path() since boot. */
size_t elf_loader__debug_call_count(void);
