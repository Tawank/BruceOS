#include "js_loader_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"
#include "core_sdk/task.h"

#include "dialog_js.h"       // IWYU pragma: export
#include "display_js.h"      // IWYU pragma: export
#include "js_stdlib.h"       // IWYU pragma: export
#include "keyboard_js.h"     // IWYU pragma: export
#include "ir_js.h"           // IWYU pragma: export
#include "notification_js.h" // IWYU pragma: export
#include "runtime_js.h"      // IWYU pragma: export
#include "serial_js.h"       // IWYU pragma: export
#include "wifi_js.h"         // IWYU pragma: export
#include "user_classes_js.h" // IWYU pragma: export

#include "mqjs_stdlib.h"

#define JS_LOADER_PATH_MAX BRUCE_STORAGE_PATH_MAX
#define JS_LOADER_SOURCE_MAX (64 * 1024u)
#define JS_LOADER_VM_MEMORY (16 * 1024u)
#define JS_LOADER_STACK_SIZE 8192u

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];
    char permission_key[BRUCE_PERMISSION_FILE_NAME_MAX];
    char *source;
    size_t source_len;
    int argc;
    char **argv;
} js_loader_task_ctx_t;

static const char *js_loader__basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool js_loader__path_is_valid(const char *path)
{
    if (path == NULL || strstr(path, "..") != NULL) {
        return false;
    }
    if (path[0] != '/' && strncmp(path, "./", 2) != 0) {
        return false;
    }
    size_t length = strlen(path);
    static const char extension[] = ".js";
    size_t extension_length = sizeof(extension) - 1;
    return length > extension_length && strcmp(path + length - extension_length, extension) == 0;
}

static bool js_loader__normalize_path(const char *path, char *out, size_t out_size)
{
    if (path == NULL || strstr(path, "..") != NULL || out_size == 0) {
        return false;
    }
    int len;
    if (path[0] == '/') {
        len = snprintf(out, out_size, "%s", path);
    } else if (strncmp(path, "./", 2) == 0) {
        len = snprintf(out, out_size, "/%s", path + 2);
    } else {
        return false;
    }
    return len > 0 && (size_t)len < out_size;
}

static void js_loader__free_task_ctx(js_loader_task_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->source != NULL) {
        memory__free(ctx->source);
    }
    app_runner__free_args(ctx->argv, ctx->argc);
    memory__free(ctx);
}

static int js_loader__load_source(const char *path, js_loader_task_ctx_t *ctx)
{
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) {
        return (int)open_result;
    }

    int result = BRUCE_OK;
    uint64_t size = 0;
    if (storage__seek(file, 0, SEEK_END, &size) != BRUCE_OK || size == 0 || size > JS_LOADER_SOURCE_MAX) {
        result = BRUCE_ERR_IO;
    } else {
        ctx->source = memory__malloc((size_t)size + 1);
        if (ctx->source == NULL) {
            result = BRUCE_ERR_NO_MEMORY;
        } else {
            ctx->source_len = (size_t)size;
            if (storage__seek(file, 0, SEEK_SET, NULL) != BRUCE_OK) {
                result = BRUCE_ERR_IO;
            } else {
                size_t total = 0;
                while (total < ctx->source_len) {
                    size_t chunk = 0;
                    if (storage__read(file, ctx->source + total, ctx->source_len - total, &chunk) != BRUCE_OK ||
                        chunk == 0) {
                        result = BRUCE_ERR_IO;
                        break;
                    }
                    total += chunk;
                }
                ctx->source[ctx->source_len] = '\0';
            }
        }
    }

    storage__close(file);
    if (result != BRUCE_OK && ctx->source != NULL) {
        memory__free(ctx->source);
        ctx->source = NULL;
        ctx->source_len = 0;
    }
    return result;
}

/* Advance *s past a leading block comment (slash-asterisk ... asterisk-slash).
 * Returns a pointer to the first code byte after the comment, or the original
 * string if no comment starts at the beginning. */
static const char *js_loader__skip_manifest_comment(const char *s)
{
    if (s == NULL || s[0] != '/' || s[1] != '*') {
        return s;
    }
    const char *end = strstr(s + 2, "*/");
    if (end == NULL) {
        return s;
    }
    return end + 2;
}

static void js__app_main(void *context)
{
    js_loader_task_ctx_t *ctx = (js_loader_task_ctx_t *)context;

    const char *script = js_loader__skip_manifest_comment(ctx->source);
    while (*script == '\n' || *script == '\r' || *script == ' ' || *script == '\t') {
        script++;
    }

    size_t mem_size = JS_LOADER_VM_MEMORY;
    uint8_t *mem_buf = memory__malloc(mem_size);
    if (mem_buf == NULL) {
        printf("[js_loader] %s: failed to allocate VM memory\n", ctx->permission_key);
        js_loader__free_task_ctx(ctx);
        return;
    }

    JSContext *js_ctx = JS_NewContext(mem_buf, mem_size, &js_stdlib);
    if (js_ctx == NULL) {
        printf("[js_loader] %s: JS_NewContext failed\n", ctx->permission_key);
        memory__free(mem_buf);
        js_loader__free_task_ctx(ctx);
        return;
    }

    JSValue val = JS_Eval(js_ctx, script, strlen(script), ctx->path, 0);
    if (JS_IsException(val)) {
        JSValue obj = JS_GetException(js_ctx);
        printf("[js_loader] %s: runtime error: ", ctx->permission_key);
        JS_PrintValueF(js_ctx, obj, JS_DUMP_LONG);
        printf("\n");
    }

    /* Optional lifecycle entry: if the script defined a global app_main(argv)
     * function, invoke it with the parsed argument vector. */
    JSValue app_main_fn = JS_GetPropertyStr(js_ctx, JS_GetGlobalObject(js_ctx), "app_main");
    if (!JS_IsException(app_main_fn) && JS_IsFunction(js_ctx, app_main_fn)) {
        JSValue argv_array = JS_NewArray(js_ctx, 0);
        if (!JS_IsException(argv_array)) {
            for (int i = 0; i < ctx->argc; i++) {
                JSValue arg_str = JS_NewString(js_ctx, ctx->argv[i]);
                if (!JS_IsException(arg_str)) {
                    JS_SetPropertyUint32(js_ctx, argv_array, (uint32_t)i, arg_str);
                }
            }
            if (JS_StackCheck(js_ctx, 3) == 0) {
                JS_PushArg(js_ctx, argv_array); /* argv */
                JS_PushArg(js_ctx, app_main_fn); /* func */
                JS_PushArg(js_ctx, JS_UNDEFINED); /* this_obj */
                JSValue ret = JS_Call(js_ctx, 1);
                if (JS_IsException(ret)) {
                    JSValue obj = JS_GetException(js_ctx);
                    printf("[js_loader] %s: app_main error: ", ctx->permission_key);
                    JS_PrintValueF(js_ctx, obj, JS_DUMP_LONG);
                    printf("\n");
                }
            } else {
                printf("[js_loader] %s: stack overflow preparing app_main call\n", ctx->permission_key);
            }
        }
    }

    JS_FreeContext(js_ctx);
    memory__free(mem_buf);
    js_loader__free_task_ctx(ctx);
}

/* Loader registry run function: called by app_runner__run_path() or by the
 * built-in "js" command. */
int js_loader__run_path(const char *path, const char *arg, bool in_background)
{
    if (!js_loader__path_is_valid(path)) {
        return BRUCE_ERR_INVALID_PATH;
    }

    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!js_loader__normalize_path(path, normalized_path, sizeof(normalized_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }

    bruce_app_inspection_t *inspection = manifest__inspect_javascript(normalized_path);
    if (inspection == NULL) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    const char *permission_key = js_loader__basename(normalized_path);

    const char *permission_names[BRUCE_MANIFEST_MAX_PERMISSIONS];
    for (size_t i = 0; i < inspection->manifest.permission_count; ++i) {
        permission_names[i] = inspection->manifest.permissions[i];
    }
    (void)permission__preflight(permission_key, permission_names, inspection->manifest.permission_count);

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) {
        memory__free(inspection);
        return (int)parse_result;
    }

    js_loader_task_ctx_t *ctx = memory__malloc(sizeof(*ctx));
    if (ctx == NULL) {
        app_runner__free_args(argv, argc);
        memory__free(inspection);
        return BRUCE_ERR_NO_MEMORY;
    }
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->path, normalized_path, sizeof(ctx->path) - 1);
    ctx->path[sizeof(ctx->path) - 1] = '\0';
    strncpy(ctx->permission_key, permission_key, sizeof(ctx->permission_key) - 1);
    ctx->permission_key[sizeof(ctx->permission_key) - 1] = '\0';
    ctx->argc = argc;
    ctx->argv = argv;

    int load_result = js_loader__load_source(ctx->path, ctx);
    if (load_result != BRUCE_OK) {
        js_loader__free_task_ctx(ctx);
        memory__free(inspection);
        return load_result;
    }

    bool gui_requested = app_runner__args_have_gui(argc, argv);

    int result = app_runner__spawn_loader_task(permission_key, gui_requested, in_background,
                                                inspection->manifest.stack_size, js__app_main, ctx);
    if (result <= 0) {
        js_loader__free_task_ctx(ctx);
    }
    memory__free(inspection);
    return result;
}

/* Built-in "js" command entry: "js ./target.js <args>..." loads the named JS
 * file and passes the remaining arguments to it. */
int js_loader__app_main(int argc, char **argv)
{
    if (argc < 2) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    const char *path = argv[1];
    bool gui_requested = app_runner__args_have_gui(argc, argv);

    char arg[BRUCE_STORAGE_PATH_MAX];
    size_t arg_len = 0;
    arg[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            if (arg_len + 1 >= sizeof(arg)) {
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
            arg[arg_len++] = ' ';
            arg[arg_len] = '\0';
        }
        size_t len = strlen(argv[i]);
        if (arg_len + len >= sizeof(arg)) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        memcpy(arg + arg_len, argv[i], len + 1);
        arg_len += len;
    }

    bruce_task_snapshot_t snapshot;
    bool in_background = false;
    if (task__snapshot(task__current_id(), &snapshot) == BRUCE_OK) {
        in_background = (snapshot.state == BRUCE_TASK_BACKGROUND);
    }
    (void)gui_requested;

    return js_loader__run_path(path, arg[0] != '\0' ? arg : NULL, in_background);
}
