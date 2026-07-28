#pragma once

/* Core-private ESP-IDF Wi-Fi implementation details.
 *
 * The public wifi__* contract -- including the wifi__network_t struct -- is
 * declared exactly once, in "core_sdk/wifi.h".  wifi_common.c includes that
 * public header directly for its own declarations instead of this file
 * redeclaring them, so there is never a second copy of the public signatures
 * that can drift out of sync (mismatched redeclarations are a compile
 * error, not a warning).
 *
 * wifi__init() is the only genuinely internal helper exposed to other Core
 * code; it does not perform a permission check because callers are public
 * wifi__* APIs that already enforce the `wifi` permission. */
#include "core_sdk/result.h"
#include <stdbool.h>

bruce_result_t wifi__init(void);
bool wifi__is_connected_internal(void);
