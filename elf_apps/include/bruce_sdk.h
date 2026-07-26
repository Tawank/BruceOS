#pragma once

/* Public SDK header for Bruce ELF applications.
 *
 * ELF apps are normal ESP-IDF projects that use project_elf() from the
 * espressif/elf_loader component.  They include this single header to get the
 * public Bruce Core API declarations and the BRUCE_APP_MANIFEST() macro.
 */

#include "core_sdk/app_runner.h"  // IWYU pragma: export
#include "core_sdk/display.h"     // IWYU pragma: export
#include "core_sdk/gpio.h"        // IWYU pragma: export
#include "core_sdk/i2c.h"         // IWYU pragma: export
#include "core_sdk/loader.h"      // IWYU pragma: export
#include "core_sdk/ir.h"          // IWYU pragma: export
#include "core_sdk/manifest.h"    // IWYU pragma: export
#include "core_sdk/memory.h"      // IWYU pragma: export
#include "core_sdk/notification.h" // IWYU pragma: export
#include "core_sdk/nrf24.h"       // IWYU pragma: export
#include "core_sdk/permission.h"  // IWYU pragma: export
#include "core_sdk/result.h"      // IWYU pragma: export
#include "core_sdk/spi.h"         // IWYU pragma: export
#include "core_sdk/storage.h"     // IWYU pragma: export
#include "core_sdk/status_icon.h" // IWYU pragma: export
#include "core_sdk/stdio.h"       // IWYU pragma: export
#include "core_sdk/task.h"        // IWYU pragma: export
#include "core_sdk/tcp.h"         // IWYU pragma: export

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
