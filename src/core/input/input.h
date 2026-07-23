#pragma once

#include "core_sdk/result.h"

/* Core-private input HAL lifecycle.  Called by main.c during bootstrap and
 * shutdown. */
bruce_result_t input__init(void);
void input__deinit(void);
