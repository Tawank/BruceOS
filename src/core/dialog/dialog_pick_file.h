#pragma once

/* GUI file/directory browser behind dialog__pick_file()/_ex() -- see
 * dialog.c's public API, which dispatches to this when the calling process
 * asked for GUI=1. */

#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

bruce_result_t dialog__gui_pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title, const bruce_dialog_render_params_t *render_params
);
