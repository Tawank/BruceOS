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
    /* When true and a window renderer is registered (see
     * dialog__set_window_renderer() below), dialog__choice() ignores every
     * other field above and instead renders inside that renderer's chrome -
     * normally the launcher's rounded border and live status bar. With no
     * renderer registered this is a no-op and the fields above apply as
     * usual. */
    bool window_chrome;
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
/* Like dialog__pick_file(), but `render_params` styles the GUI listing the
 * same way it styles dialog__choice() (NULL behaves exactly like
 * dialog__pick_file()). Ignored on non-GUI/terminal picks. */
bruce_result_t dialog__pick_file_ex(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const bruce_dialog_render_params_t *render_params
);

/* Window chrome: a pluggable "look" (rounded border, live status bar, ...)
 * that dialog__choice() draws around a choice list when render_params has
 * `window_chrome=true`. Core doesn't own that look - the launcher registers
 * it once at GUI startup via dialog__set_window_renderer(), and every
 * foreground GUI process sharing the display then gets the same window by
 * requesting window_chrome. If nothing is registered, window_chrome is
 * ignored and the normal full-screen layout is used instead. */
typedef void (*bruce_dialog_window_draw_fn_t)(void *context);

typedef struct {
    /* Space to reserve around the choice viewport for the border/status bar. */
    int padding_top;
    int padding_right;
    int padding_bottom;
    int padding_left;
    /* Font size window-chrome content should use; <= 0 falls back to 1. */
    int text_size;
    /* Paints the full window frame. Called once per dialog__choice() call,
     * before the choice viewport is first drawn, inside an active frame. */
    bruce_dialog_window_draw_fn_t draw_border;
    /* Repaints just the live parts (e.g. clock/battery). Called right after
     * draw_border and then again every status_refresh_interval_ms while the
     * dialog stays open. May be NULL. */
    bruce_dialog_window_draw_fn_t draw_status;
    uint32_t status_refresh_interval_ms;
} bruce_dialog_window_renderer_t;

/* Registers the renderer dialog__choice() uses for window_chrome requests;
 * the struct is copied, so it may be stack-allocated. Pass NULL to clear a
 * previously registered renderer. */
void dialog__set_window_renderer(const bruce_dialog_window_renderer_t *renderer, void *context);

/* Returns the render_params dialog__choice() uses when `render_params` is
 * NULL (full screen, bordered title/footer bars, themed colors), with
 * `text_size` substituted (falls back to the normal default when <= 0).
 * Useful for a caller that wants that standard look at a different font
 * size. */
bruce_dialog_render_params_t dialog__default_render_params(int text_size);

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
