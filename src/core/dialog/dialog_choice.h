#pragma once

/* The GUI choice-list renderer behind every dialog__choice*()/_ex() variant
 * and dialog__gui_pick_file() (dialog_pick_file.c) -- see dialog.c's public
 * API, which dispatches to it when the calling process asked for GUI=1. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

/* Registers `renderer` (a launcher-owned window chrome/status hook -- see
 * core_sdk/dialog.h) as what dialog__gui_choice() draws around a
 * render_launcher list instead of its own full-bleed/bordered layout. Pass
 * NULL to clear it. */
void dialog__set_window_renderer(const bruce_dialog_window_renderer_t *renderer, void *context);

/* Optional periodic poll a caller can have dialog__gui_choice() invoke
 * between redraws -- see dialog__choice_poll()/dialog__choice_poll_launcher()
 * in dialog.c, the only callers that populate one. */
typedef struct {
    uint32_t interval_ms;
    bruce_dialog_poll_callback_t callback;
    void *context;
    bool *out_complete;
} dialog__choice_poll_t;

bruce_result_t dialog__gui_choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected, const bruce_dialog_render_params_t *render_params, const dialog__choice_poll_t *poll
);

/* Backs dialog__choice_search_ex()/dialog__choice_search_launcher() (see
 * core_sdk/dialog.h for the public contract) -- a dialog__gui_choice() list
 * with a live-filtering query box in place of a static message, so a caller
 * gets a searchable choice list without rolling its own. */
bruce_result_t dialog__gui_choice_search(
    const char *title, const char *prompt, const bruce_dialog_choice_t *choices, size_t choice_count, char *query,
    size_t query_capacity, size_t *out_selected, const bruce_dialog_render_params_t *render_params
);
