#pragma once

#include <stddef.h>
#include "core_sdk/app_runner.h"

/*
 * Built-in ELF loader module (see migration_plan.md, "Loader modules" /
 * "ELF contract").  This is an ordinary built-in module - it includes only
 * core_sdk/... headers and gets no private-Core-header exemption, unlike
 * modules/selftest.
 */
void elf_loader__init(void);
int elf_loader__app_main(int argc, char **argv);
/* Test-only observability for modules/selftest (exempt from the
 * core_sdk-only-headers rule); not part of any public contract.  Counts
 * completed ELF opens since boot. */
size_t elf_loader__debug_call_count(void);
