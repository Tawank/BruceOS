#include "core_sdk/js.h"

#include <string.h>

#include "core/js/js.h"
#include "core/storage/storage.h"
#include "core_sdk/result.h"

static size_t s_call_count;

size_t js__debug_call_count(void)
{
    return s_call_count;
}

/* Placeholder Core implementation.  Stage 3 (A7) replaces this file wholesale
 * with the real mQuickJS runner, optional leading manifest parsing, and
 * task-owned VM allocation - see core_sdk/js.h.  For now it only proves out
 * the documented path-validation and existence-check contract, which is
 * enough for AppRunner's named-resolution order to be observable. */
int js__run_path(const char *path, const char *arg, bool in_background)
{
    (void)arg;
    (void)in_background;
    s_call_count++;

    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL) {
        return BRUCE_ERR_INVALID_PATH;
    }

    size_t length = strlen(path);
    static const char extension[] = ".js";
    size_t extension_length = sizeof(extension) - 1;
    if (length <= extension_length || strcmp(path + length - extension_length, extension) != 0) {
        return BRUCE_ERR_INVALID_PATH;
    }

    if (!storage__exists(path)) {
        return BRUCE_ERR_NOT_FOUND;
    }

    return BRUCE_ERR_UNSUPPORTED;
}
