#pragma once

#include <stdbool.h>

#include "core_sdk/process.h"

/* Switch to / Kill / Cancel for one process; returns true when the user chose to switch to it. */
bool process_app__menu(const bruce_process_snapshot_t *process, bool can_switch);

/* Live tile grid of background GUI apps. */
int process_app__preview(void);

/* Task manager: every process with its CPU and memory use. */
int process_list__run(void);
