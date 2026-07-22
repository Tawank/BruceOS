#pragma once

/* Core-private ESP-IDF Wi-Fi implementation details.
 *
 * The public wifi__* contract -- including the wifi__network_t struct -- is
 * declared exactly once, in "core_sdk/wifi.h".  wifi_common.c includes that
 * public header directly for its own declarations instead of this file
 * redeclaring them, so there is never a second copy of the public signatures
 * that can drift out of sync (mismatched redeclarations are a compile
 * error, not a warning). */
int wifi__init(void);
