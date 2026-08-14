#pragma once

#include <stdbool.h>
/* Register the Bruce SDK native import module after WAMR initialization and
 * before loading any module which imports from "bruce_sdk". Safe to call more
 * than once for the lifetime of one WAMR runtime. */
bool wasm_bruce_sdk__register(void);
