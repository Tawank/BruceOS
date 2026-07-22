#include "dialog.h"

#include "core_sdk/dialog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/task/task.h"

static dialog__test_choice_provider_t s_test_provider;
static bool s_last_call_was_gui;

void dialog__test_set_choice_provider(dialog__test_choice_provider_t provider)
{
    s_test_provider = provider;
}

bool dialog__test_last_call_was_gui(void)
{
    return s_last_call_was_gui;
}

/* Renderer selection comes from the calling task's preserved --gui launch
 * context (see migration_BruceIDF.md, "Dialog and task interaction"), not
 * from any dialog-call argument. A caller with no Core task context (e.g.
 * Core itself, before any task exists) is treated as terminal. */
static bool dialog__current_task_wants_gui(void)
{
    bool gui_requested = false;
    (void)task_registry__current_context(NULL, NULL, 0, &gui_requested);
    return gui_requested;
}

static const char *dialog__kind_label(bruce_dialog_kind_t kind)
{
    switch (kind) {
        case BRUCE_DIALOG_INFO: return "info";
        case BRUCE_DIALOG_SUCCESS: return "success";
        case BRUCE_DIALOG_WARNING: return "warning";
        case BRUCE_DIALOG_ERROR: return "error";
        default: return "info";
    }
}

bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message)
{
    bool gui = dialog__current_task_wants_gui();
    s_last_call_was_gui = gui;

    /* Stage 6 (A9) replaces this with a real display widget for GUI tasks;
     * until then every renderer prints to the console. */
    printf("[dialog:%s:%s] %s%s%s\n", gui ? "gui" : "term", dialog__kind_label(kind), title != NULL ? title : "",
           title != NULL && message != NULL ? ": " : "", message != NULL ? message : "");
    return BRUCE_OK;
}

bruce_result_t dialog__choice(const char *title, const char *message, const bruce_dialog_choice_t *choices,
                              size_t choice_count, size_t *out_selected)
{
    if (choices == NULL || choice_count == 0 || out_selected == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bool gui = dialog__current_task_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_provider != NULL) {
        return s_test_provider(title, message, choices, choice_count, out_selected);
    }

    /* Stage 6 (A9) replaces the GUI branch with a real display widget; both
     * renderers use the console for now. */
    if (title != NULL) printf("%s\n", title);
    if (message != NULL) printf("%s\n", message);
    for (size_t i = 0; i < choice_count; ++i) {
        printf("%u. %s\n", (unsigned int)(i + 1), choices[i].label != NULL ? choices[i].label : "");
    }
    printf("pick: ");
    fflush(stdout);

    char line[16];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        return BRUCE_ERR_CANCELLED;
    }
    char *end = NULL;
    long picked = strtol(line, &end, 10);
    if (end == line || picked < 1 || (size_t)picked > choice_count) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_selected = (size_t)(picked - 1);
    return BRUCE_OK;
}

/* dialog__pick_file() and the text-viewer functions are Stage 6 (A9) work
 * (they need real Storage-picker UI and a display-backed viewer resource);
 * core_sdk/dialog.h already declares their final signatures so callers can
 * be written against them now. */

bruce_result_t dialog__pick_file(const char *initial_path, const char *extension_filter, char *out_path,
                                 size_t out_path_size)
{
    (void)initial_path;
    (void)extension_filter;
    (void)out_path;
    (void)out_path_size;
    return BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t dialog__create_text_viewer(const char *title, const char *text, bruce_viewer_id_t *out_viewer)
{
    (void)title;
    (void)text;
    (void)out_viewer;
    return BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t dialog__viewer_set_text(bruce_viewer_id_t viewer, const char *text)
{
    (void)viewer;
    (void)text;
    return BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t dialog__viewer_scroll(bruce_viewer_id_t viewer, int lines)
{
    (void)viewer;
    (void)lines;
    return BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t dialog__viewer_close(bruce_viewer_id_t viewer)
{
    (void)viewer;
    return BRUCE_ERR_UNSUPPORTED;
}
