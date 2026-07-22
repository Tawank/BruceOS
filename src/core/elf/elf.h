#pragma once

#include <stddef.h>

/* Core-private test observability only (used by modules/selftest, which is
 * exempt from the core_sdk-only rule).  Not part of the public SDK contract
 * in core_sdk/elf.h.  Counts completed calls into elf__run_path() since
 * boot, so a test can prove which loader AppRunner actually reached without
 * relying on both placeholder loaders returning the same BRUCE_ERR_* value. */
size_t elf__debug_call_count(void);
