#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

bruce_result_t
dialog__time_gui_run(const char *title, bool with_seconds, uint8_t max_hours, uint32_t *seconds);
