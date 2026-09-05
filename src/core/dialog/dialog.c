#include "dialog.h"
#include "dialog_choice.h"
#include "dialog_gui_common.h"
#include "dialog_input.h"
#include "dialog_internal.h"
#include "dialog_message.h"
#include "dialog_pick_file.h"
#include "dialog_term.h"

#include "core/process/process.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Test provider hooks (see core/dialog/dialog.h)                             */
/* -------------------------------------------------------------------------- */

static dialog__test_choice_provider_t s_test_choice_provider;
static dialog__test_input_provider_t s_test_input_provider;
static dialog__test_pick_file_provider_t s_test_pick_file_provider;
static bool s_last_call_was_gui;

void dialog__test_set_choice_provider(dialog__test_choice_provider_t provider) {
    s_test_choice_provider = provider;
}

void dialog__test_set_input_provider(dialog__test_input_provider_t provider) {
    s_test_input_provider = provider;
}

void dialog__test_set_pick_file_provider(dialog__test_pick_file_provider_t provider) {
    s_test_pick_file_provider = provider;
}

bool dialog__test_last_call_was_gui(void) { return s_last_call_was_gui; }

/* -------------------------------------------------------------------------- */
/* Renderer selection                                                         */
/* -------------------------------------------------------------------------- */

/* Declared in dialog_internal.h -- dialog_viewer.c's own public API entry
 * points share this same GUI-or-terminal decision, so they call these two
 * instead of duplicating the process_registry__current_context() lookup and
 * the s_last_call_was_gui bookkeeping below. */
bool dialog__current_process_wants_gui(void) {
    bool gui_requested = false;
    (void)process_registry__current_context(NULL, NULL, 0, &gui_requested);
    return gui_requested;
}

void dialog__note_last_call_was_gui(bool gui) { s_last_call_was_gui = gui; }

bruce_dialog_render_params_t dialog__default_render_params(int text_size) {
    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    bruce_dialog_render_params_t params = {0};
    params.render_borders = true;
    params.text_size = text_size > 0 ? text_size : dialog__default_list_text_size();
    params.background_color = bg;
    /* pri, not `text`: this is also the color list rows (folders/files in
     * the picker) draw their label in, over background_color, so it should
     * stay the theme accent. The title bar's own text needs a color that
     * survives being filled with pri underneath it - dialog__gui_choice()
     * (dialog_choice.c) picks that separately rather than borrowing this
     * field. */
    params.text_color = pri;
    return params;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (gui) { return dialog__gui_message(kind, title, message); }
    return dialog__term_message(kind, title, message);
}

bruce_result_t dialog__message_show(bruce_dialog_kind_t kind, const char *title, const char *message) {
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (gui) { return dialog__gui_message_show(kind, title, message); }
    return dialog__term_message(kind, title, message);
}

bruce_result_t dialog__choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    if (choices == NULL || choice_count == 0 || out_selected == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_choice_provider != NULL) {
        return s_test_choice_provider(title, message, choices, choice_count, out_selected);
    }

    if (gui) {
        bruce_dialog_render_params_t params = dialog__default_render_params(2);
        params.padding_top = 8;
        params.padding_bottom = 8;
        params.padding_left = 12;
        params.padding_right = 12;
        return dialog__gui_choice(title, message, choices, choice_count, out_selected, &params, NULL);
    }
    return dialog__term_choice(title, message, choices, choice_count, out_selected);
}

static const bruce_dialog_render_params_t s_window_launcher = {.render_launcher = true};

static bruce_result_t dialog__choice_poll_common(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    uint32_t poll_interval_ms, bruce_dialog_poll_callback_t poll_callback, void *poll_context,
    bruce_dialog_cleanup_callback_t cleanup_callback, size_t *out_selected, bool *out_poll_complete, bool launcher
) {
    if (choices == NULL || choice_count == 0 || poll_interval_ms == 0 || poll_callback == NULL ||
        out_selected == NULL || out_poll_complete == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_poll_complete = false;
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;
    bruce_result_t result;
    if (s_test_choice_provider != NULL) {
        result = s_test_choice_provider(title, message, choices, choice_count, out_selected);
    } else if (gui) {
        dialog__choice_poll_t poll = {
            .interval_ms = poll_interval_ms,
            .callback = poll_callback,
            .context = poll_context,
            .out_complete = out_poll_complete,
        };
        if (launcher) {
            result = dialog__gui_choice(title, message, choices, choice_count, out_selected, &s_window_launcher, &poll);
        } else {
            bruce_dialog_render_params_t params = dialog__default_render_params(2);
            params.padding_top = 8;
            params.padding_bottom = 8;
            params.padding_left = 12;
            params.padding_right = 12;
            result = dialog__gui_choice(title, message, choices, choice_count, out_selected, &params, &poll);
        }
    } else {
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (cleanup_callback != NULL) { cleanup_callback(poll_context); }
    return result;
}

bruce_result_t dialog__choice_poll(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    uint32_t poll_interval_ms, bruce_dialog_poll_callback_t poll_callback, void *poll_context,
    bruce_dialog_cleanup_callback_t cleanup_callback, size_t *out_selected, bool *out_poll_complete
) {
    return dialog__choice_poll_common(
        title, message, choices, choice_count, poll_interval_ms, poll_callback, poll_context, cleanup_callback,
        out_selected, out_poll_complete, false
    );
}

bruce_result_t dialog__choice_poll_launcher(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    uint32_t poll_interval_ms, bruce_dialog_poll_callback_t poll_callback, void *poll_context,
    bruce_dialog_cleanup_callback_t cleanup_callback, size_t *out_selected, bool *out_poll_complete
) {
    return dialog__choice_poll_common(
        title, message, choices, choice_count, poll_interval_ms, poll_callback, poll_context, cleanup_callback,
        out_selected, out_poll_complete, true
    );
}

bruce_result_t dialog__choice_launcher(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    if (choices == NULL || choice_count == 0 || out_selected == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_choice_provider != NULL) {
        return s_test_choice_provider(title, message, choices, choice_count, out_selected);
    }

    if (gui) {
        return dialog__gui_choice(title, message, choices, choice_count, out_selected, &s_window_launcher, NULL);
    }
    return dialog__term_choice(title, message, choices, choice_count, out_selected);
}

bruce_result_t dialog__choice_ex(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected, const bruce_dialog_render_params_t *render_params
) {
    if (choices == NULL || choice_count == 0 || out_selected == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (gui && render_params != NULL &&
        (render_params->padding_top < 0 || render_params->padding_right < 0 ||
         render_params->padding_bottom < 0 || render_params->padding_left < 0 ||
         render_params->padding_left + render_params->padding_right >= display__width() ||
         render_params->padding_top + render_params->padding_bottom >= display__height())) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    if (s_test_choice_provider != NULL) {
        return s_test_choice_provider(title, message, choices, choice_count, out_selected);
    }

    if (gui) {
        return dialog__gui_choice(title, message, choices, choice_count, out_selected, render_params, NULL);
    }
    return dialog__term_choice(title, message, choices, choice_count, out_selected);
}

bruce_result_t dialog__pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title
) {
    return dialog__pick_file_ex(initial_path, extension_filter, out_path, out_path_size, title, NULL);
}

bruce_result_t dialog__pick_file_ex(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title, const bruce_dialog_render_params_t *render_params
) {
    if (out_path == NULL || out_path_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_pick_file_provider != NULL) {
        return s_test_pick_file_provider(initial_path, extension_filter, out_path, out_path_size);
    }

    if (gui) {
        return dialog__gui_pick_file(
            initial_path, extension_filter, out_path, out_path_size, title, render_params
        );
    }
    return dialog__term_pick_file(initial_path, extension_filter, out_path, out_path_size);
}

bruce_result_t dialog__text_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_input_provider != NULL) {
        return s_test_input_provider(title, prompt, initial_text, mask_input, buffer, buffer_size);
    }

    if (gui) {
        return dialog__input_gui_run(
            title, prompt, initial_text, mask_input, buffer, buffer_size, DIALOG__INPUT_TEXT
        );
    }
    return dialog__term_input(title, prompt, initial_text, mask_input, buffer, buffer_size, NULL);
}

bruce_result_t dialog__hex_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_input_provider != NULL) {
        return s_test_input_provider(title, prompt, initial_text, false, buffer, buffer_size);
    }

    if (gui) {
        return dialog__input_gui_run(
            title, prompt, initial_text, false, buffer, buffer_size, DIALOG__INPUT_HEX
        );
    }
    return dialog__term_input(title, prompt, initial_text, false, buffer, buffer_size, dialog__validate_hex);
}

bruce_result_t dialog__number_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_input_provider != NULL) {
        return s_test_input_provider(title, prompt, initial_text, false, buffer, buffer_size);
    }

    if (gui) {
        return dialog__input_gui_run(
            title, prompt, initial_text, false, buffer, buffer_size, DIALOG__INPUT_NUMBER
        );
    }
    return dialog__term_input(
        title, prompt, initial_text, false, buffer, buffer_size, dialog__validate_number
    );
}
