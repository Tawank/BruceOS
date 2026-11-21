#pragma once

#include <stdbool.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

/* Core event-loop lifecycle. Initialized before applications are started. */
bruce_result_t event_loop__init(void);
void event_loop__deinit(void);

/* Process-registry hook. Called with the process registry locked; this function must
 * not call back into process APIs. */
void event_loop__foreground_changed(bruce_process_id_t process_id);

/* While true, injected input is dropped: the press that wakes the device from
 * sleep must not reach the app. */
void event_loop__set_input_blocked(bool blocked);
