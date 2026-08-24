#pragma once

#include <stdbool.h>

#include "core_sdk/disk.h" // IWYU pragma: export

/* Mounts sd0 at /sdcard at boot, before core/process has any registered
 * process to satisfy disk__mount()'s built-in-caller check. Not part of
 * core_sdk: only main.c's startup sequence should call this. */
bool disk__mount_sd_boot(void);
