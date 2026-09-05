#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/process.h"
#include "core_sdk/result.h"

/**
 * @brief Messages, menus, input dialogs, and text viewers.
 *
 * Not permission-gated. Rendering is chosen from the process's launch
 * context, never by an app-specific renderer.
 */

typedef enum {
    BRUCE_DIALOG_INFO,
    BRUCE_DIALOG_SUCCESS,
    BRUCE_DIALOG_WARNING,
    BRUCE_DIALOG_ERROR,
} bruce_dialog_kind_t;

typedef struct {
    const char *label;      /* Primary row text. */
    const char *value;      /* Stable action/identifier for the choice. */
    const char *icon_name;  /* Optional built-in icon name, drawn on GUI. */
    const char *right_text; /* Optional short GUI/terminal status text. */
} bruce_dialog_choice_t;

/** Buffer size for an icon name, e.g. bruce_dialog_render_params_t.icon_for_path's out_icon. */
#define BRUCE_DIALOG_ICON_NAME_MAX 32

typedef void (*bruce_dialog_render_callback_t)(void *context);

/**
 * @brief Performs one short, non-blocking unit of work for dialog__choice_poll().
 *
 * Set @p out_complete to true when the operation has finished. Returning an
 * error closes the dialog and returns that error to the caller.
 */
typedef bruce_result_t (*bruce_dialog_poll_callback_t)(void *context, bool *out_complete);

/** @brief Releases work started for dialog__choice_poll(). */
typedef void (*bruce_dialog_cleanup_callback_t)(void *context);

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
    bool render_launcher;
    /* Extra vertical space between the title/message block and the first
     * list row, e.g. so a selected first row doesn't visually fuse with the
     * title bar above it. 0 (the default) draws the list flush against
     * them, matching every existing caller. */
    int list_gap;
    /* When true, a SELECT press held for at least DIALOG__LONG_PRESS_MS
     * before release resolves as a long press instead of an immediate
     * selection - dialog__choice_ex()/dialog__pick_file_ex() still return
     * BRUCE_OK with the highlighted row in *out_selected either way, but
     * report which one happened through `out_long_press` below. False (the
     * default) keeps every existing caller returning the instant a SELECT
     * press arrives, exactly as before this field existed. GUI only;
     * ignored on a terminal/non-GUI dialog. */
    bool long_press_enabled;
    /* Set to true/false on every long_press_enabled selection, reporting
     * whether it was a long press. May be NULL if the caller only set
     * long_press_enabled to change dialog__pick_file_ex()'s folder
     * behavior below without needing to know afterward which kind of
     * press it was. Left untouched when long_press_enabled is false. */
    bool *out_long_press;
    /* Set when the returned long-press selection was the synthetic `[..]`
     * row rather than a real file or directory. The returned path is the
     * directory that was being displayed. */
    bool *out_parent_entry;
    /* Optional per-entry icon override for dialog__pick_file_ex()'s GUI
     * listing. Called with each real row's full path (never the synthetic
     * `[..]` row) before the picker's own default icon_name logic runs;
     * fills `out_icon` (a BRUCE_DIALOG_ICON_NAME_MAX-byte buffer) and
     * returns true to use it, or returns false to fall through to the
     * default (a per-extension icon for a file, "folder" for a directory).
     * Writing into a caller-owned buffer rather than returning a pointer
     * keeps every listed row's icon independently valid for as long as the
     * listing stays on screen, since the picker builds every row's choice
     * entry before any of them are drawn. Ignored by every other
     * dialog__* call. May be NULL. */
    bool (*icon_for_path)(const char *path, bool is_directory, char *out_icon, size_t out_icon_size, void *context);
    void *icon_for_path_context;
} bruce_dialog_render_params_t;

/**
 * @brief Shows a simple modal message dialog.
 *
 * Dialog APIs return BRUCE_OK or BRUCE_ERR_CANCELLED, BRUCE_ERR_BUSY,
 * BRUCE_ERR_INVALID_ARGUMENT, or another BRUCE_ERR_* result.
 *
 * @param kind Message severity/style.
 * @param title Optional short title shown at the top of the dialog.
 * @param message Body text shown in the dialog.
 */
bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message);

/**
 * @brief Draws a message dialog like dialog__message(), but returns
 * immediately instead of waiting for a keypress.
 *
 * For a slow blocking call (opening an archive, starting a scan, ...) with
 * no natural point to update or poll a status from: draws this once so the
 * screen isn't left looking frozen for that whole stretch, then the caller
 * does its blocking work. Nothing needs to dismiss it afterward -- the
 * caller's next dialog__* call (an error message, a result list, ...)
 * repaints the full screen anyway, replacing it.
 *
 * @param kind Message severity/style.
 * @param title Optional short title shown at the top of the dialog.
 * @param message Body text shown in the dialog.
 */
bruce_result_t dialog__message_show(bruce_dialog_kind_t kind, const char *title, const char *message);

/**
 * @brief Shows a choice-list dialog and waits for a selection.
 *
 * @param title Optional short title shown at the top of the dialog.
 * @param message Optional body text shown above the choice list.
 * @param choices Choices to list.
 * @param choice_count Number of entries in choices.
 * @param out_selected Receives the index of the selected choice.
 */
bruce_result_t dialog__choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
);

/**
 * @brief Shows a GUI choice dialog while periodically polling caller-owned work.
 *
 * The poll callback runs immediately and then no more often than
 * @p poll_interval_ms. It must not block. When it sets @p out_complete true,
 * this function returns BRUCE_OK with @p out_poll_complete set true. A user
 * choice also returns BRUCE_OK, with @p out_poll_complete set false and
 * @p out_selected set to that choice. Back returns BRUCE_ERR_CANCELLED.
 *
 * This is GUI-only, except that the dialog test choice provider may supply a
 * choice. The cleanup callback, when present, runs exactly once after every
 * return path following argument validation.
 */
bruce_result_t dialog__choice_poll(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    uint32_t poll_interval_ms, bruce_dialog_poll_callback_t poll_callback, void *poll_context,
    bruce_dialog_cleanup_callback_t cleanup_callback, size_t *out_selected, bool *out_poll_complete
);

/**
 * @brief Like dialog__choice_poll(), styled for use from the launcher.
 */
bruce_result_t dialog__choice_poll_launcher(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    uint32_t poll_interval_ms, bruce_dialog_poll_callback_t poll_callback, void *poll_context,
    bruce_dialog_cleanup_callback_t cleanup_callback, size_t *out_selected, bool *out_poll_complete
);

/**
 * @brief Like dialog__choice(), styled for use from the launcher.
 *
 * @param title Optional short title shown at the top of the dialog.
 * @param message Optional body text shown above the choice list.
 * @param choices Choices to list.
 * @param choice_count Number of entries in choices.
 * @param out_selected Receives the index of the selected choice.
 */
bruce_result_t dialog__choice_launcher(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
);

/**
 * @brief Choice dialog with GUI render styling.
 *
 * `render_params` only affects GUI rendering. NULL uses the full display
 * with the standard title and footer bars. Padding limits the choice
 * viewport, and `render_borders=false` renders a plain size-1 title without
 * those bars. `text_size` and the colors style the choices; selected rows
 * invert the two colors. When `render_callback` is set, it runs inside each
 * active dialog frame after the choice viewport is drawn.
 * `refresh_interval_ms` requests periodic frames so caller-owned pixels
 * outside the viewport can update.
 *
 * @param title Optional short title shown at the top of the dialog.
 * @param message Optional body text shown above the choice list.
 * @param choices Choices to list.
 * @param choice_count Number of entries in choices.
 * @param out_selected Receives the index of the selected choice.
 * @param render_params GUI render styling, or NULL for the standard look.
 */
bruce_result_t dialog__choice_ex(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected, const bruce_dialog_render_params_t *render_params
);

/**
 * @brief Like dialog__choice_ex(), but with a live-filtering search box.
 *
 * Below the title (if any), an editable query line is drawn in place of a
 * static message: printable keys append to it, Backspace/Delete remove from
 * its end, and the list below is live-filtered down to the choices whose
 * label contains the current query as a case-insensitive substring (an
 * empty query matches everything), re-selecting the first match whenever it
 * changes. UP/DOWN/SELECT/BACK still drive the list itself. `out_selected`
 * is always an index into the original, unfiltered `choices` -- not into
 * whatever subset happened to be showing when the user picked one.
 *
 * `query` is a caller-owned in/out buffer of `query_capacity` bytes: pass it
 * pre-seeded (e.g. with the keypress that opened the search) to start
 * already filtered, or empty to start showing everything. It's left holding
 * whatever the user last typed when this returns, BRUCE_OK or
 * BRUCE_ERR_CANCELLED alike.
 *
 * GUI only on GUI vs. terminal dispatch is unaffected: a non-GUI process
 * falls back to the plain numbered dialog__choice() list, `query` unused.
 *
 * @param title Optional short title shown at the top of the dialog.
 * @param prompt Optional label shown ahead of the query text, e.g. "Find: " (NULL uses that default).
 * @param choices Choices to search and list.
 * @param choice_count Number of entries in choices.
 * @param query In/out buffer holding the current query text.
 * @param query_capacity Size of `query` in bytes, including the nul terminator.
 * @param out_selected Receives the index (into `choices`) of the selected choice.
 * @param render_params GUI render styling, or NULL for the standard look.
 */
bruce_result_t dialog__choice_search_ex(
    const char *title, const char *prompt, const bruce_dialog_choice_t *choices, size_t choice_count, char *query,
    size_t query_capacity, size_t *out_selected, const bruce_dialog_render_params_t *render_params
);

/**
 * @brief Like dialog__choice_search_ex(), styled for use from the launcher.
 */
bruce_result_t dialog__choice_search_launcher(
    const char *title, const char *prompt, const bruce_dialog_choice_t *choices, size_t choice_count, char *query,
    size_t query_capacity, size_t *out_selected
);

/**
 * @brief Opens a file/directory picker.
 *
 * `title`, if non-NULL/non-empty, is shown in the GUI picker's top bar
 * ahead of the current directory, e.g. "NES - /" or "Filemanager - /roms"
 * (NULL/"" shows just the directory); the browsed directory no longer gets
 * its own subtitle line underneath. Ignored on non-GUI/terminal picks.
 *
 * @param initial_path Directory the picker starts browsing from.
 * @param extension_filter Optional filter limiting which file extensions are shown.
 * @param out_path Receives the picked file/directory path.
 * @param out_path_size Size of out_path in bytes.
 * @param title Optional short title shown ahead of the current directory in the GUI picker.
 */
bruce_result_t dialog__pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title
);

/**
 * @brief Like dialog__pick_file(), but with GUI render styling.
 *
 * `render_params` additionally styles the GUI listing the same way it
 * styles dialog__choice() (NULL behaves exactly like dialog__pick_file())
 * - its `render_callback`/`render_callback_context` are reserved for the
 * picker's own use (it draws the current volume's name and used/total
 * space in the bottom bar) and are overridden if set. `icon_for_path`/
 * `icon_for_path_context`, if set, are consulted for every listed entry
 * before the picker's default icon choice - see their doc comment above.
 * Ignored on non-GUI/terminal picks.
 *
 * `render_params->long_press_enabled` additionally changes what a long
 * press on a *directory* row does: instead of descending into it, the
 * picker returns immediately with that directory's own path in `out_path`
 * (still BRUCE_OK, `*render_params->out_long_press` true) - the same way a
 * short press on a file returns that file's path - so a caller can offer
 * folder-level actions (rename/delete/info, ...) the way it already can
 * for a picked file. A short press on a directory still descends into it
 * exactly as before, long_press_enabled or not. A long press on the
 * synthetic `[..]` row returns the directory currently being displayed and
 * sets `*render_params->out_parent_entry`; a short press navigates upward,
 * or returns BRUCE_ERR_CANCELLED when used at root.
 *
 * @param initial_path Directory the picker starts browsing from.
 * @param extension_filter Optional filter limiting which file extensions are shown.
 * @param out_path Receives the picked file/directory path.
 * @param out_path_size Size of out_path in bytes.
 * @param title Optional short title shown ahead of the current directory in the GUI picker.
 * @param render_params GUI render styling, or NULL for dialog__pick_file()'s look.
 */
bruce_result_t dialog__pick_file_ex(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title, const bruce_dialog_render_params_t *render_params
);

/**
 * @brief Draws the frame around a choice dialog.
 *
 * (rounded border, live status bar, ...) drawn when render_params has
 * `window_chrome=true`. Core doesn't own that look - the launcher registers
 * it once at GUI startup via dialog__set_window_renderer(), and every
 * foreground GUI process sharing the display then gets the same window by
 * requesting window_chrome. If nothing is registered, window_chrome is
 * ignored and the normal full-screen layout is used instead.
 */
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

/**
 * @brief Registers the renderer dialog__choice() uses for window_chrome requests.
 *
 * The struct is copied, so it may be stack-allocated. Pass NULL to clear a
 * previously registered renderer.
 *
 * @param renderer Window-chrome renderer to register, or NULL to clear it.
 * @param context Opaque pointer passed back to the renderer's draw callbacks.
 */
void dialog__set_window_renderer(const bruce_dialog_window_renderer_t *renderer, void *context);

/**
 * @brief Returns the render_params dialog__choice() uses when `render_params` is NULL.
 *
 * (full screen, bordered title/footer bars, themed colors), with
 * `text_size` substituted (falls back to the normal default when <= 0).
 * Useful for a caller that wants that standard look at a different font
 * size.
 *
 * @param text_size Font size to substitute into the returned params.
 */
bruce_dialog_render_params_t dialog__default_render_params(int text_size);

/**
 * @brief Text input dialog accepting printable ASCII characters and space.
 *
 * Fills the caller-owned buffer with the entered string (NUL-terminated,
 * truncated to buffer_size - 1), and returns BRUCE_OK or
 * BRUCE_ERR_CANCELLED.
 *
 * @param title Optional short title shown at the top of the dialog.
 * @param prompt Optional short prompt shown at the top of the dialog.
 * @param initial_text Optional pre-filled text, or NULL.
 * @param mask_input Hides typed characters on GUI (useful for passwords) and disables terminal echo on CLI.
 * @param buffer Caller-owned buffer to receive the entered text.
 * @param buffer_size Size of buffer in bytes.
 */
bruce_result_t dialog__text_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size
);

/**
 * @brief Text input dialog accepting only 0-9, A-F, a-f.
 *
 * Fills the caller-owned buffer with the entered string (NUL-terminated,
 * truncated to buffer_size - 1), and returns BRUCE_OK or
 * BRUCE_ERR_CANCELLED.
 *
 * @param title Optional short title shown at the top of the dialog.
 * @param prompt Optional short prompt shown at the top of the dialog.
 * @param initial_text Optional pre-filled text, or NULL.
 * @param buffer Caller-owned buffer to receive the entered text.
 * @param buffer_size Size of buffer in bytes.
 */
bruce_result_t dialog__hex_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
);

/**
 * @brief Text input dialog accepting only 0-9 and at most one '.' or '-'.
 *
 * Fills the caller-owned buffer with the entered string (NUL-terminated,
 * truncated to buffer_size - 1), and returns BRUCE_OK or
 * BRUCE_ERR_CANCELLED.
 *
 * @param title Optional short title shown at the top of the dialog.
 * @param prompt Optional short prompt shown at the top of the dialog.
 * @param initial_text Optional pre-filled text, or NULL.
 * @param buffer Caller-owned buffer to receive the entered text.
 * @param buffer_size Size of buffer in bytes.
 */
bruce_result_t dialog__number_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
);

/**
 * @brief Opens a scrollable text viewer dialog.
 *
 * @param title Optional short title shown at the top of the viewer.
 * @param text Text to display.
 * @param out_viewer Receives the new viewer's id.
 */
bruce_result_t dialog__create_text_viewer(const char *title, const char *text, bruce_viewer_id_t *out_viewer);

/**
 * @brief Replaces the text shown in an open viewer.
 *
 * @param viewer Viewer to update.
 * @param text New text to display.
 */
bruce_result_t dialog__viewer_set_text(bruce_viewer_id_t viewer, const char *text);

/**
 * @brief Sets the GUI body font multiplier (1..8) and redraws the viewer.
 *
 * @param viewer Viewer to update.
 * @param text_size New font size multiplier (1..8).
 */
bruce_result_t dialog__viewer_set_text_size(bruce_viewer_id_t viewer, int text_size);

/**
 * @brief Scrolls an open viewer by a number of lines.
 *
 * @param viewer Viewer to scroll.
 * @param lines Number of lines to scroll by (negative scrolls up).
 */
bruce_result_t dialog__viewer_scroll(bruce_viewer_id_t viewer, int lines);

/**
 * @brief Closes an open text viewer.
 *
 * @param viewer Viewer to close.
 */
bruce_result_t dialog__viewer_close(bruce_viewer_id_t viewer);
