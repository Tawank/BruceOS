#pragma once

#include <stddef.h>

#include "core_sdk/result.h"
#include "core_sdk/task.h"

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

/* Dialog APIs return BRUCE_OK or BRUCE_ERR_CANCELLED, BRUCE_ERR_BUSY,
 * BRUCE_ERR_INVALID_ARGUMENT, or another BRUCE_ERR_* result.  Rendering is
 * chosen from the task's launch context, never by an app-specific renderer. */
bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message);
bruce_result_t dialog__choice(const char *title, const char *message, const bruce_dialog_choice_t *choices,
                              size_t choice_count, size_t *out_selected);
bruce_result_t dialog__pick_file(const char *initial_path, const char *extension_filter, char *out_path,
                                 size_t out_path_size);
bruce_result_t dialog__create_text_viewer(const char *title, const char *text, bruce_viewer_id_t *out_viewer);
bruce_result_t dialog__viewer_set_text(bruce_viewer_id_t viewer, const char *text);
bruce_result_t dialog__viewer_scroll(bruce_viewer_id_t viewer, int lines);
bruce_result_t dialog__viewer_close(bruce_viewer_id_t viewer);
