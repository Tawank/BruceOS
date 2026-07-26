#pragma once

#include "core_sdk/result.h"
#include "core_sdk/task.h"

/* Core-private input HAL lifecycle.  Called by main.c during bootstrap and
 * shutdown. */
bruce_result_t input__init(void);
void input__deinit(void);

/* Task-registry hook. Called with the task registry locked; this function must
 * not call back into task APIs. */
void input__foreground_changed(bruce_task_id_t task_id);
