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

/* esp_timer milliseconds of the last input injected (not blocked); 0 before any. */
uint64_t event_loop__last_input_ms(void);
/* Counts as input for idle timing, without any event (waking from sleep). */
void event_loop__note_input(void);
