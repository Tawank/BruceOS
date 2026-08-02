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
int elf_loader__run_path(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);

/* Test-only observability for modules/selftest (exempt from the
 * core_sdk-only-headers rule); not part of any public contract.  Counts
 * completed calls into elf_loader__run_path() since boot. */
size_t elf_loader__debug_call_count(void);
