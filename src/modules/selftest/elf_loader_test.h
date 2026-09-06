#pragma once

#include <stdbool.h>

bool selftest__run_elf_loader_xip_case(void);
bool selftest__run_elf_loader_stdio_case(void);
bool selftest__run_elf_loader_libc_case(void);
bool selftest__run_elf_loader_time_case(void);
bool selftest__run_elf_loader_posix_case(void);
bool selftest__run_elf_loader_exit_case(void);
bool selftest__run_wasm_loader_case(void);
