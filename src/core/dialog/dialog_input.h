#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

typedef enum {
    DIALOG__INPUT_TEXT,
    DIALOG__INPUT_HEX,
    DIALOG__INPUT_NUMBER,
} dialog__input_kind_t;

bruce_result_t dialog__input_gui_run(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size, dialog__input_kind_t kind
);
