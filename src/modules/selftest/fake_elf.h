#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Test-only helper (modules/selftest is exempt from the core_sdk-only-
 * headers rule): builds a minimal, valid ELF32 image containing a
 * non-loadable ".bruce.manifest" section with the given canonical manifest
 * fields, and writes it atomically to `path`.  Used to exercise the real
 * modules/loaders/elf/ module without needing a toolchain-built binary.
 *
 * `app_icon_all_zero` is always used (128 zero bytes, base64-encoded
 * internally) since no test currently needs a non-trivial icon.  Returns
 * false on any internal buffer-size failure.
 */
bool selftest__write_fake_elf(const char *path, const char *app_name, const char *const *permissions,
                               size_t permission_count);
