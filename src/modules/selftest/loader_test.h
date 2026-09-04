#pragma once

#include <stdbool.h>

bool selftest__run_manifest_parse_case(void);
bool selftest__run_manifest_parse_named_icon_case(void);
bool selftest__run_loader_registry_extensibility_case(void);
bool selftest__run_elf_loader_case(void);
bool selftest__run_reclaim_handoff_case(void);
bool selftest__run_wasm_manifest_case(void);
bool selftest__run_js_loader_case(void);
