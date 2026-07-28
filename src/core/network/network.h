#pragma once

#include "core_sdk/result.h"

/* Initializes the transport-neutral ESP-NETIF/lwIP runtime without starting
 * Wi-Fi or requiring a radio permission. */
bruce_result_t network__init(void);
