#pragma once

#include <stddef.h>

#include "core_sdk/app_runner.h"

void wasm_loader__init(void);
int wasm_loader__app_main(int argc, char **argv);
size_t wasm_loader__debug_call_count(void);
