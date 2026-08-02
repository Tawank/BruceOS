#pragma once

/* Core-private permission-store test hook. Every real permission__* API is
 * declared exactly once in "core_sdk/permission.h"; this header must never
 * redeclare any of it. It exists solely so modules/selftest (the one
 * built-in exempted from the core_sdk-only-headers rule) can reset the
 * on-disk/in-memory permission store between test cases. No other Core
 * service or built-in should include this header. */

#include <stdbool.h>

/* Clears every in-memory decision and deletes /config/permissions.json so the next
 * permission__check()/permission__preflight() call starts from a clean
 * slate. Returns false only if the file existed and could not be removed. */
bool permission__test_reset(void);
