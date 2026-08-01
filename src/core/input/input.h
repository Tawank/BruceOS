#pragma once

#include "core_sdk/result.h"
#include "core_sdk/process.h"

/* Core-private input HAL shutdown lifecycle. */
void input__deinit(void);

/* Process-registry hook. Called with the process registry locked; this function must
 * not call back into process APIs. */
void input__foreground_changed(bruce_process_id_t process_id);
