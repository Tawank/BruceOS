#include "filemanager_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#define FILEMANAGER_PREVIEW_MAX 4096

static bool filemanager__resume_after_handoff(void) {
    bruce_process_snapshot_t snapshot;
    bruce_process_id_t self = process__current_id();
    if (self == BRUCE_PROCESS_ID_INVALID || process__snapshot(self, &snapshot) != BRUCE_OK ||
        snapshot.state != BRUCE_PROCESS_BACKGROUND) {
        return false;
    }
    do {
        if (runtime__delay(20) != BRUCE_OK || process__snapshot(self, &snapshot) != BRUCE_OK) return false;
    } while (snapshot.state == BRUCE_PROCESS_BACKGROUND);
    return snapshot.state == BRUCE_PROCESS_FOREGROUND;
}

static const char *filemanager__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool filemanager__is_editable_text(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot != NULL &&
           (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".json") == 0 ||
            strcasecmp(dot, ".conf") == 0);
}

static bruce_result_t filemanager__read_preview(const char *path, char **out_text, bool *out_truncated) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    char *text = memory__calloc(FILEMANAGER_PREVIEW_MAX + 1, 1);
    if (text == NULL) {
        (void)storage__close(file);
        return BRUCE_ERR_NO_MEMORY;
    }

    size_t read_size = 0;
    result = storage__read(file, text, FILEMANAGER_PREVIEW_MAX, &read_size);
    if (result == BRUCE_OK) {
        char extra;
        size_t extra_size = 0;
        result = storage__read(file, &extra, 1, &extra_size);
        *out_truncated = result == BRUCE_OK && extra_size > 0;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        memory__free(text);
        return result;
    }

    for (size_t i = 0; i < read_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\0' || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) text[i] = '.';
    }
    text[read_size] = '\0';
    *out_text = text;
    return BRUCE_OK;
}

static bruce_result_t filemanager__view_file(const char *path, bool gui) {
    if (image__is_supported_path(path) || filemanager__is_editable_text(path)) {
        int process = app_runner__run_path(path, NULL, true);
        if (process <= 0) return (bruce_result_t)process;
        return process__wait((bruce_process_id_t)process, UINT32_MAX);
    }

    char *text = NULL;
    bool truncated = false;
    bruce_result_t result = filemanager__read_preview(path, &text, &truncated);
    if (result != BRUCE_OK) return result;

    if (!gui) {
        stdio__printf("%s%s\n", text, truncated ? "\n[preview truncated]" : "");
        memory__free(text);
        return BRUCE_OK;
    }

    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    result = dialog__create_text_viewer(filemanager__basename(path), text, &viewer);
    memory__free(text);
    if (result != BRUCE_OK) return result;

    (void)input__flush();
    for (;;) {
        bruce_input_event_t event;
        result = input__read(&event, 100);
        if (result == BRUCE_ERR_NOT_FOREGROUND && filemanager__resume_after_handoff()) continue;
        if (result == BRUCE_ERR_NOT_FOREGROUND) break;
        if (result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) continue;

        if (event.code == BRUCE_INPUT_CODE_UP) {
            (void)dialog__viewer_scroll(viewer, -1);
        } else if (event.code == BRUCE_INPUT_CODE_DOWN) {
            (void)dialog__viewer_scroll(viewer, 1);
        } else if (event.code == BRUCE_INPUT_CODE_LEFT) {
            (void)dialog__viewer_scroll(viewer, -5);
        } else if (event.code == BRUCE_INPUT_CODE_RIGHT) {
            (void)dialog__viewer_scroll(viewer, 5);
        } else if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B ||
                   event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A) {
            break;
        }
    }
    return dialog__viewer_close(viewer);
}

static bruce_result_t filemanager__show_info(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    (void)storage__close(file);
    if (result != BRUCE_OK) return result;

    char message[BRUCE_STORAGE_PATH_MAX + 48];
    snprintf(message, sizeof(message), "%s\n%llu bytes", path, (unsigned long long)size);
    return dialog__message(BRUCE_DIALOG_INFO, "File info", message);
}

static void filemanager__show_error(const char *action, bruce_result_t result) {
    char message[80];
    snprintf(message, sizeof(message), "%s failed (%d)", action, result);
    (void)dialog__message(BRUCE_DIALOG_ERROR, "File manager", message);
}

int filemanager_app_main(int argc, char **argv) {
    bool gui = app_runner__args_have_gui(argc, argv);
    if (gui && !app_runner__args_have_background(argc, argv)) {
        bruce_result_t foreground = process__to_foreground();
        if (foreground != BRUCE_OK) return foreground;
    }
    const bruce_dialog_choice_t actions[] = {
        {.label = "Open / view", .value = "view"},
        {.label = "File info",   .value = "info"},
        {.label = "Run",         .value = "run" },
        {.label = "Back",        .value = "back"},
    };
    /* File manager stays full screen (no window chrome) but reads better
     * with a larger font than the default. */
    const bruce_dialog_render_params_t action_params = dialog__default_render_params(2);

    for (;;) {
        char path[BRUCE_STORAGE_PATH_MAX];
        bruce_result_t result = dialog__pick_file_ex("/", NULL, path, sizeof(path), &action_params);
        if (result == BRUCE_ERR_CANCELLED && filemanager__resume_after_handoff()) {
            (void)input__flush();
            continue;
        }
        if (result == BRUCE_ERR_CANCELLED) return 0;
        if (result != BRUCE_OK) {
            filemanager__show_error("Browse", result);
            return result;
        }

        size_t selected = 0;
        result = dialog__choice(
            "File manager", path, actions, sizeof(actions) / sizeof(actions[0]), &selected, &action_params
        );
        if (result == BRUCE_ERR_CANCELLED && filemanager__resume_after_handoff()) {
            (void)input__flush();
            continue;
        }
        if (result == BRUCE_ERR_CANCELLED || selected == 3) continue;
        if (result != BRUCE_OK) {
            filemanager__show_error("Action", result);
            continue;
        }

        if (selected == 0) {
            result = filemanager__view_file(path, gui);
        } else if (selected == 1) {
            result = filemanager__show_info(path);
        } else {
            int process = app_runner__run_path(path, gui ? "--gui" : "", true);
            result = process > 0 ? BRUCE_OK : (bruce_result_t)process;
        }
        if (result != BRUCE_OK) filemanager__show_error(actions[selected].label, result);
        (void)input__flush();
    }
}
