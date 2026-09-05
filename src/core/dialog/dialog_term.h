#pragma once

/* Terminal (non-GUI) renderers for every dialog__* kind -- see dialog.c's
 * public API, which dispatches to these when the calling process didn't ask
 * for GUI=1. */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

bruce_result_t dialog__term_message(bruce_dialog_kind_t kind, const char *title, const char *message);

bruce_result_t dialog__term_choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
);

bruce_result_t dialog__term_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size, bool (*validate)(const char *text, size_t len)
);

/* validate callbacks dialog.c passes to dialog__term_input() for
 * dialog__hex_input()'s/dialog__number_input()'s terminal fallback. */
bool dialog__validate_hex(const char *text, size_t len);
bool dialog__validate_number(const char *text, size_t len);

bruce_result_t dialog__term_pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size
);
