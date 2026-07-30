#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

typedef enum {
    BRUCE_DIALOG_INFO,
    BRUCE_DIALOG_SUCCESS,
    BRUCE_DIALOG_WARNING,
    BRUCE_DIALOG_ERROR,
} bruce_dialog_kind_t;

typedef struct {
    const char *label;
    const char *value;
} bruce_dialog_choice_t;

typedef void (*bruce_dialog_render_callback_t)(void *context);

typedef struct {
    int padding_top;
    int padding_right;
    int padding_bottom;
    int padding_left;
    bool render_borders;
    int text_size;
    uint16_t background_color;
    uint16_t text_color;
    uint32_t refresh_interval_ms;
    bruce_dialog_render_callback_t render_callback;
    void *render_callback_context;
} bruce_dialog_render_params_t;

/* Dialog APIs return BRUCE_OK or BRUCE_ERR_CANCELLED, BRUCE_ERR_BUSY,
 * BRUCE_ERR_INVALID_ARGUMENT, or another BRUCE_ERR_* result.  Rendering is
 * chosen from the process's launch context, never by an app-specific renderer. */
bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message);
/* `render_params` only affects GUI rendering. NULL uses the full display with
 * the standard title and footer bars. Padding limits the choice viewport, and
 * `render_borders=false` renders a plain size-1 title without those bars.
 * `text_size` and the colors style the choices; selected rows invert the two
 * colors. When `render_callback` is set, it runs inside each active dialog
 * frame after the choice viewport is drawn. `refresh_interval_ms` requests
 * periodic frames so caller-owned pixels outside the viewport can update. */
bruce_result_t dialog__choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected, const bruce_dialog_render_params_t *render_params
);
bruce_result_t dialog__pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size
);

/* Text input dialogs.
 *
 * All three functions prompt the user for text, fill the caller-owned buffer
 * with the entered string (NUL-terminated, truncated to buffer_size - 1), and
 * return BRUCE_OK or BRUCE_ERR_CANCELLED.
 *
 * `title` and `prompt` are optional short strings shown at the top of the
 * dialog.  `initial_text` may be NULL.  `mask_input` hides typed characters on
 * GUI (useful for passwords) and disables terminal echo on CLI.
 *
 * dialog__text_input accepts printable ASCII characters and space.
 * dialog__hex_input accepts only 0-9, A-F, a-f.
 * dialog__number_input accepts only 0-9 and at most one '.' or '-'.
 */
bruce_result_t dialog__text_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size
);
bruce_result_t dialog__hex_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
);
bruce_result_t dialog__number_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
);

bruce_result_t dialog__create_text_viewer(const char *title, const char *text, bruce_viewer_id_t *out_viewer);
bruce_result_t dialog__viewer_set_text(bruce_viewer_id_t viewer, const char *text);
bruce_result_t dialog__viewer_scroll(bruce_viewer_id_t viewer, int lines);
bruce_result_t dialog__viewer_close(bruce_viewer_id_t viewer);
