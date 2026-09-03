#include "dialog_test.h"

#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core_sdk/dialog.h"
#include "core_sdk/storage.h"

/* ------------------------------------------------------------------------ */
/* Mock input provider                                                       */
/* ------------------------------------------------------------------------ */

static struct {
    const char *result;
    bool mask_observed;
} s_input_mock;

static bruce_result_t selftest__dialog_input_provider(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *out_buffer,
    size_t buffer_size
) {
    (void)title;
    (void)prompt;
    (void)initial_text;
    s_input_mock.mask_observed = mask_input;
    if (s_input_mock.result == NULL) { return BRUCE_ERR_CANCELLED; }
    snprintf(out_buffer, buffer_size, "%s", s_input_mock.result);
    return BRUCE_OK;
}

/* ------------------------------------------------------------------------ */
/* Mock pick_file provider                                                   */
/* ------------------------------------------------------------------------ */

static const char *s_pick_file_result;

static bool s_choice_poll_callback_called;
static bool s_choice_poll_cleanup_called;

static bruce_result_t selftest__dialog_choice_provider(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title;
    (void)message;
    (void)choices;
    if (choice_count != 1) { return BRUCE_ERR_INVALID_ARGUMENT; }
    *out_selected = 0;
    return BRUCE_OK;
}

static bruce_result_t selftest__dialog_choice_poll_callback(void *context, bool *out_complete) {
    (void)context;
    s_choice_poll_callback_called = true;
    *out_complete = false;
    return BRUCE_OK;
}

static void selftest__dialog_choice_poll_cleanup(void *context) {
    (void)context;
    s_choice_poll_cleanup_called = true;
}

static bruce_result_t selftest__dialog_pick_file_provider(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size
) {
    (void)initial_path;
    (void)extension_filter;
    if (s_pick_file_result == NULL) { return BRUCE_ERR_CANCELLED; }
    snprintf(out_path, out_path_size, "%s", s_pick_file_result);
    return BRUCE_OK;
}

/* ------------------------------------------------------------------------ */
/* Cases                                                                     */
/* ------------------------------------------------------------------------ */

bool selftest__run_dialog_text_input_case(void) {
    s_input_mock.result = "hello";
    s_input_mock.mask_observed = false;
    dialog__test_set_input_provider(selftest__dialog_input_provider);

    char buffer[64] = {0};
    bruce_result_t result = dialog__text_input("Title", "Prompt", "initial", true, buffer, sizeof(buffer));

    dialog__test_set_input_provider(NULL);

    if (result != BRUCE_OK) {
        printf("[selftest] dialog/text_input: FAIL, result=%d\n", result);
        return false;
    }
    if (strcmp(buffer, "hello") != 0) {
        printf("[selftest] dialog/text_input: FAIL, got '%s'\n", buffer);
        return false;
    }
    if (!s_input_mock.mask_observed) {
        printf("[selftest] dialog/text_input: FAIL, mask_input not observed\n");
        return false;
    }

    printf("[selftest] dialog/text_input: OK\n");
    return true;
}

bool selftest__run_dialog_hex_input_case(void) {
    s_input_mock.result = "DEADBEEF";
    dialog__test_set_input_provider(selftest__dialog_input_provider);

    char buffer[64] = {0};
    bruce_result_t result = dialog__hex_input("Hex", "Enter hex", NULL, buffer, sizeof(buffer));

    dialog__test_set_input_provider(NULL);

    if (result != BRUCE_OK || strcmp(buffer, "DEADBEEF") != 0) {
        printf("[selftest] dialog/hex_input: FAIL, result=%d, got '%s'\n", result, buffer);
        return false;
    }

    printf("[selftest] dialog/hex_input: OK\n");
    return true;
}

bool selftest__run_dialog_number_input_case(void) {
    s_input_mock.result = "123.45";
    dialog__test_set_input_provider(selftest__dialog_input_provider);

    char buffer[64] = {0};
    bruce_result_t result = dialog__number_input("Number", "Enter number", NULL, buffer, sizeof(buffer));

    dialog__test_set_input_provider(NULL);

    if (result != BRUCE_OK || strcmp(buffer, "123.45") != 0) {
        printf("[selftest] dialog/number_input: FAIL, result=%d, got '%s'\n", result, buffer);
        return false;
    }

    printf("[selftest] dialog/number_input: OK\n");
    return true;
}

bool selftest__run_dialog_pick_file_case(void) {
    s_pick_file_result = "/apps/test.elf";
    dialog__test_set_pick_file_provider(selftest__dialog_pick_file_provider);

    char path[BRUCE_STORAGE_PATH_MAX] = {0};
    bruce_result_t result = dialog__pick_file("/apps", ".elf", path, sizeof(path), NULL);

    dialog__test_set_pick_file_provider(NULL);

    if (result != BRUCE_OK || strcmp(path, "/apps/test.elf") != 0) {
        printf("[selftest] dialog/pick_file: FAIL, result=%d, got '%s'\n", result, path);
        return false;
    }

    printf("[selftest] dialog/pick_file: OK\n");
    return true;
}

bool selftest__run_dialog_viewer_case(void) {
    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    bruce_result_t result = dialog__create_text_viewer("Test", "Line 1\nLine 2\nLine 3", &viewer);
    if (result != BRUCE_OK) {
        printf("[selftest] dialog/viewer: FAIL, create returned %d\n", result);
        return false;
    }

    result = dialog__viewer_scroll(viewer, 1);
    if (result != BRUCE_OK) {
        printf("[selftest] dialog/viewer: FAIL, scroll returned %d\n", result);
        dialog__viewer_close(viewer);
        return false;
    }

    result = dialog__viewer_set_text_size(viewer, 2);
    if (result != BRUCE_OK || dialog__viewer_set_text_size(viewer, 0) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] dialog/viewer: FAIL, text size returned %d\n", result);
        dialog__viewer_close(viewer);
        return false;
    }

    result = dialog__viewer_close(viewer);
    if (result != BRUCE_OK) {
        printf("[selftest] dialog/viewer: FAIL, close returned %d\n", result);
        return false;
    }

    /* Closing again should fail. */
    if (dialog__viewer_close(viewer) == BRUCE_OK) {
        printf("[selftest] dialog/viewer: FAIL, double close succeeded\n");
        return false;
    }

    printf("[selftest] dialog/viewer: OK\n");
    return true;
}

bool selftest__run_dialog_message_show_case(void) {
    /* Non-blocking: this terminal-mode run has no key to wait for, so a
     * hang here means dialog__message_show() forgot and blocked anyway. */
    bruce_result_t result = dialog__message_show(BRUCE_DIALOG_INFO, "Title", "Loading...\nmessage");
    if (result != BRUCE_OK) {
        printf("[selftest] dialog/message_show: FAIL, result=%d\n", result);
        return false;
    }

    printf("[selftest] dialog/message_show: OK\n");
    return true;
}

bool selftest__run_dialog_choice_poll_case(void) {
    const bruce_dialog_choice_t choices[] = {{.label = "Back", .value = "back"}};
    size_t selected = 1;
    bool complete = true;
    s_choice_poll_callback_called = false;
    s_choice_poll_cleanup_called = false;
    dialog__test_set_choice_provider(selftest__dialog_choice_provider);
    bruce_result_t result = dialog__choice_poll(
        "Polling", NULL, choices, 1, 10, selftest__dialog_choice_poll_callback, NULL,
        selftest__dialog_choice_poll_cleanup, &selected, &complete
    );
    dialog__test_set_choice_provider(NULL);

    if (result != BRUCE_OK || selected != 0 || complete || s_choice_poll_callback_called ||
        !s_choice_poll_cleanup_called) {
        printf("[selftest] dialog/choice_poll: FAIL, result=%d\n", result);
        return false;
    }

    printf("[selftest] dialog/choice_poll: OK\n");
    return true;
}
