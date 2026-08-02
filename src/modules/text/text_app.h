#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "core_sdk/app_runner.h"

int text_app_main(int argc, char **argv);
int text__run_path(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);
