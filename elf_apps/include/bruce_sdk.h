#pragma once

/* Public SDK header for Bruce ELF applications.
 *
 * ELF apps are normal ESP-IDF projects that use project_elf() from the
 * espressif/elf_loader component.  They include this single header to get the
 * public Bruce Core API declarations and the BRUCE_APP_MANIFEST() macro.
 */

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"
#include "core_sdk/task.h"

/* Embed the canonical manifest JSON in the non-loadable .bruce.manifest
 * section.  The JSON string must be valid UTF-8 and contain the required
 * appName, appIcon (base64 128 bytes), coreAbiVersion, and stackSize fields.
 *
 * Example:
 *   BRUCE_APP_MANIFEST("{\"appName\":\"My app\",\"appIcon\":\"...\","
 *                       "\"coreAbiVersion\":1,\"stackSize\":8192}");
 */
#define BRUCE_APP_MANIFEST(json) \
    __attribute__((section(".bruce.manifest"), used)) \
    static const char bruce_manifest[] = json
