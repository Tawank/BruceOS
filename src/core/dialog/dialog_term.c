#include "dialog_term.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

static const char *dialog__kind_label(bruce_dialog_kind_t kind) {
    switch (kind) {
        case BRUCE_DIALOG_INFO: return "info";
        case BRUCE_DIALOG_SUCCESS: return "success";
        case BRUCE_DIALOG_WARNING: return "warning";
        case BRUCE_DIALOG_ERROR: return "error";
        default: return "info";
    }
}

bruce_result_t dialog__term_message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    stdio__printf(
        "[dialog:%s:%s] %s%s%s\n",
        "term",
        dialog__kind_label(kind),
        title != NULL ? title : "",
        title != NULL && message != NULL ? ": " : "",
        message != NULL ? message : ""
    );
    return BRUCE_OK;
}

static int dialog__term_read_line(char *buffer, size_t buffer_size, bool mask_input) {
    return stdio__read_line(buffer, buffer_size, mask_input);
}

bruce_result_t dialog__term_choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    if (title != NULL) { stdio__printf("%s\n", title); }
    if (message != NULL) { stdio__printf("%s\n", message); }
    for (size_t i = 0; i < choice_count; ++i) {
        stdio__printf("%u. %s", (unsigned int)(i + 1), choices[i].label != NULL ? choices[i].label : "");
        if (choices[i].right_text != NULL && choices[i].right_text[0] != '\0') {
            stdio__printf("  %s", choices[i].right_text);
        }
        stdio__printf("\n");
    }
    stdio__printf("pick: ");

    char line[16];
    if (dialog__term_read_line(line, sizeof(line), false) < 0) { return BRUCE_ERR_CANCELLED; }
    char *end = NULL;
    long picked = strtol(line, &end, 10);
    if (end == line || picked < 1 || (size_t)picked > choice_count) { return BRUCE_ERR_INVALID_ARGUMENT; }
    *out_selected = (size_t)(picked - 1);
    return BRUCE_OK;
}

bruce_result_t dialog__term_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size, bool (*validate)(const char *text, size_t len)
) {
    if (title != NULL) { stdio__printf("%s\n", title); }
    if (prompt != NULL) { stdio__printf("%s", prompt); }
    if (initial_text != NULL && initial_text[0] != '\0') { stdio__printf(" [%s]", initial_text); }
    stdio__printf(": ");

    char tmp[256];
    size_t tmp_size = buffer_size < sizeof(tmp) ? buffer_size : sizeof(tmp);
    if (tmp_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    int len = dialog__term_read_line(tmp, tmp_size, mask_input);
    if (len < 0) { return BRUCE_ERR_CANCELLED; }

    if (tmp[0] == '\0' && initial_text != NULL) {
        snprintf(buffer, buffer_size, "%s", initial_text);
    } else {
        snprintf(buffer, buffer_size, "%s", tmp);
    }

    if (validate != NULL && !validate(buffer, strlen(buffer))) {
        stdio__printf("Invalid input.\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    return BRUCE_OK;
}

bool dialog__validate_hex(const char *text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (!isxdigit((unsigned char)text[i])) { return false; }
    }
    return true;
}

bool dialog__validate_number(const char *text, size_t len) {
    bool has_dot = false;
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '-') {
            if (i != 0) { return false; }
        } else if (c == '.') {
            if (has_dot) { return false; }
            has_dot = true;
        } else if (!isdigit((unsigned char)c)) {
            return false;
        }
    }
    return true;
}

bruce_result_t dialog__term_pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size
) {
    const char *path = initial_path != NULL && initial_path[0] != '\0' ? initial_path : "/";
    stdio__printf("Enter file path");
    if (extension_filter != NULL) { stdio__printf(" (%s)", extension_filter); }
    stdio__printf(" [%s]: ", path);

    char line[BRUCE_STORAGE_PATH_MAX];
    int len = dialog__term_read_line(line, sizeof(line), false);
    if (len < 0) { return BRUCE_ERR_CANCELLED; }

    if (line[0] == '\0') {
        snprintf(out_path, out_path_size, "%s", path);
    } else {
        snprintf(out_path, out_path_size, "%s", line);
    }
    return BRUCE_OK;
}
