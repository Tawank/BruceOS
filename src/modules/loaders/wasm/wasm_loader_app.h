#pragma once

#include <stddef.h>

#include "core_sdk/app_runner.h"

void wasm_loader__init(void);
int wasm_loader__app_main(int argc, char **argv);
int wasm_loader__run_path(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);

size_t wasm_loader__debug_call_count(void);
