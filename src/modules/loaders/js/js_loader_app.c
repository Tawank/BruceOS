#include "js_loader_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"

#include "audio_js.h"   // IWYU pragma: export
#include "dialog_js.h"  // IWYU pragma: export
#include "display_js.h" // IWYU pragma: export
#include "icon_js.h"    // IWYU pragma: export
#include "ir_js.h"      // IWYU pragma: export
#include "js_source.h"
#include "js_stdlib.h"       // IWYU pragma: export
#include "keyboard_js.h"     // IWYU pragma: export
#include "notification_js.h" // IWYU pragma: export
#include "runtime_js.h"      // IWYU pragma: export
#include "serial_js.h"       // IWYU pragma: export
#include "user_classes_js.h" // IWYU pragma: export
#include "wifi_js.h"         // IWYU pragma: export

#include "mqjs_stdlib.h"

#define JS_LOADER_PATH_MAX BRUCE_STORAGE_PATH_MAX
#define JS_LOADER_SOURCE_MAX (32 * 1024u)
#define JS_LOADER_VM_MEMORY_MIN (32 * 1024u)
#define JS_LOADER_VM_MEMORY_PREFERRED 100000u
#define JS_LOADER_STACK_SIZE 4096u

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];
    char permission_key[BRUCE_PERMISSION_FILE_NAME_MAX];
    js_source_t source;
    int argc;
    char **argv;
} js_loader_process_ctx_t;

static const char *js_loader__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool js_loader__path_is_valid(const char *path) {
    if (path == NULL || strstr(path, "..") != NULL) { return false; }
    if (path[0] != '/' && strncmp(path, "./", 2) != 0) { return false; }
    size_t length = strlen(path);
    static const char extension[] = ".js";
    size_t extension_length = sizeof(extension) - 1;
    return length > extension_length && strcmp(path + length - extension_length, extension) == 0;
}

static bool js_loader__normalize_path(const char *path, char *out, size_t out_size) {
    if (path == NULL || strstr(path, "..") != NULL || out_size == 0) { return false; }
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

static void js_loader__free_process_ctx(js_loader_process_ctx_t *ctx) {
    if (ctx == NULL) { return; }
    js_source__release(&ctx->source);
    app_runner__free_args(ctx->argv, ctx->argc);
    free(ctx);
}

/* Advance *s past a leading block comment (slash-asterisk ... asterisk-slash).
 * Returns a pointer to the first code byte after the comment, or the original
 * string if no comment starts at the beginning. */
static size_t js_loader__skip_manifest_comment(const uint8_t *source, size_t source_len) {
    if (source == NULL || source_len < 2 || source[0] != '/' || source[1] != '*') return 0;
    for (size_t i = 2; i + 1 < source_len; ++i) {
        if (source[i] == '*' && source[i + 1] == '/') return i + 2;
    }
    return 0;
}

static size_t js_loader__vm_memory_size(void) {
    bruce_memory_stats_t stats;
    if (memory__get_stats(&stats) != BRUCE_OK) return JS_LOADER_VM_MEMORY_PREFERRED;

    size_t largest = stats.internal_largest_block;
    if (largest >= 150000u) return JS_LOADER_VM_MEMORY_PREFERRED;
    if (largest <= 8192u) return 0;
    size_t adaptive = largest / 2u < 65536u ? largest - 8192u : 65536u;
    return adaptive >= JS_LOADER_VM_MEMORY_MIN ? adaptive : 0;
}

static void js_loader__print_exception(JSContext *js_ctx, JSValue exception) {
    JSValue text = JS_ToString(js_ctx, exception);
    if (!JS_IsException(text)) {
        JSCStringBuf text_buf;
        const char *message = JS_ToCString(js_ctx, text, &text_buf);
        if (message != NULL) {
            printf("%s", message);
            return;
        }
    }
    JS_PrintValueF(js_ctx, exception, JS_DUMP_LONG);
}

static void js__app_main(void *context) {
    js_loader_process_ctx_t *ctx = (js_loader_process_ctx_t *)context;

    bruce_result_t adopt_result = js_source__adopt(&ctx->source);
    if (adopt_result != BRUCE_OK) {
        printf("[js_loader] %s: failed to adopt source (%d)\n", ctx->permission_key, (int)adopt_result);
        js_loader__free_process_ctx(ctx);
        return;
    }
    js_source_t *source = &ctx->source;

    size_t script_offset = js_loader__skip_manifest_comment(source->data, source->size);
    while (script_offset < source->size &&
           (source->data[script_offset] == '\n' || source->data[script_offset] == '\r' ||
            source->data[script_offset] == ' ' || source->data[script_offset] == '\t')) {
        script_offset++;
    }
    const char *script = (const char *)source->data + script_offset;
    size_t script_len = source->size - script_offset;

    size_t mem_size = js_loader__vm_memory_size();
    if (mem_size == 0) {
        printf("[js_loader] %s: insufficient writable memory for VM\n", ctx->permission_key);
        js_loader__free_process_ctx(ctx);
        return;
    }
    uint8_t *mem_buf = memory__malloc(mem_size);
    if (mem_buf == NULL) {
        printf("[js_loader] %s: failed to allocate VM memory\n", ctx->permission_key);
        js_loader__free_process_ctx(ctx);
        return;
    }

    JSContext *js_ctx = JS_NewContext(mem_buf, mem_size, &js_stdlib);
    if (js_ctx == NULL) {
        printf("[js_loader] %s: JS_NewContext failed\n", ctx->permission_key);
        memory__free(mem_buf);
        js_loader__free_process_ctx(ctx);
        return;
    }

    JSValue val = JS_Eval(js_ctx, script, script_len, ctx->path, 0);
    js_source__release(source);
    if (JS_IsException(val)) {
        JSValue obj = JS_GetException(js_ctx);
        printf("[js_loader] %s: runtime error: ", ctx->permission_key);
        js_loader__print_exception(js_ctx, obj);
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
                JS_PushArg(js_ctx, argv_array);   /* argv */
                JS_PushArg(js_ctx, app_main_fn);  /* func */
                JS_PushArg(js_ctx, JS_UNDEFINED); /* this_obj */
                JSValue ret = JS_Call(js_ctx, 1);
                if (JS_IsException(ret)) {
                    JSValue obj = JS_GetException(js_ctx);
                    printf("[js_loader] %s: app_main error: ", ctx->permission_key);
                    js_loader__print_exception(js_ctx, obj);
                    printf("\n");
                }
            } else {
                printf("[js_loader] %s: stack overflow preparing app_main call\n", ctx->permission_key);
            }
        }
    }

    JS_FreeContext(js_ctx);
    memory__free(mem_buf);
    js_loader__free_process_ctx(ctx);
}

/* Loader registry run function: called by app_runner__run_path() or by the
 * built-in "js" command. */
int js_loader__run_path(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    if (!js_loader__path_is_valid(path)) { return BRUCE_ERR_INVALID_PATH; }

    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!js_loader__normalize_path(path, normalized_path, sizeof(normalized_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }

    bruce_app_inspection_t *inspection = manifest__inspect_javascript(normalized_path);
    if (inspection == NULL) { return BRUCE_ERR_MANIFEST_INVALID; }

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

    js_loader_process_ctx_t *ctx = malloc(sizeof(*ctx));
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

    bruce_result_t source_result =
        js_source__load_transferable(ctx->path, JS_LOADER_SOURCE_MAX, &ctx->source);
    if (source_result != BRUCE_OK) {
        printf(
            "[js_loader] %s: failed to load source (%d, external %d)\n",
            ctx->permission_key,
            (int)source_result,
            (int)ctx->source.external_result
        );
        js_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return source_result;
    }

    bool gui_requested = app_runner__environment_requests_gui(environment, environment_count);

    bruce_loader_t parent_image = ctx->source.external;
    int result = app_runner__spawn_loader_process(
        permission_key,
        gui_requested,
        mode,
        inspection->manifest.stack_size,
        environment,
        environment_count,
        js__app_main,
        ctx
    );
    if (result <= 0) {
        js_loader__free_process_ctx(ctx);
    } else if (parent_image.memory.handle != 0) {
        for (;;) {
            bruce_process_snapshot_t snapshot;
            bruce_result_t snapshot_result = process__snapshot(result, &snapshot);
            if (snapshot_result != BRUCE_OK) {
                (void)loader__release_image(&parent_image);
                break;
            }
            if (snapshot.resource_count > 0) break;
            if (runtime__delay(1) != BRUCE_OK) break;
        }
    }
    memory__free(inspection);
    return result;
}

/* Built-in "js" command entry: "js ./target.js <args>..." loads the named JS
 * file and passes the remaining arguments to it. */
static bool js_loader__append_arg(char *out, size_t out_size, size_t *used, const char *value) {
    size_t needed = *used > 0 ? 1u : 0u;
    needed += 2;
    for (const char *p = value; *p != '\0'; ++p) needed += (*p == '\\' || *p == '"') ? 2u : 1u;
    if (needed >= out_size - *used) return false;
    if (*used > 0) out[(*used)++] = ' ';
    out[(*used)++] = '"';
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '"') out[(*used)++] = '\\';
        out[(*used)++] = *p;
    }
    out[(*used)++] = '"';
    out[*used] = '\0';
    return true;
}

int js_loader__app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) { return BRUCE_ERR_NO_MEMORY; }
    ap_set_helptext(parser, "Load and run a JavaScript application.");
    ap_add_required_arg(parser, "path", "JavaScript file to load");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP) { return 0; }
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    const char *path = ap_get_arg(parser, "path");

    char arg[BRUCE_STORAGE_PATH_MAX];
    size_t arg_len = 0;
    arg[0] = '\0';
    int parsed_argc = ap_count_args(parser);
    for (int i = 1; i < parsed_argc; i++) {
        const char *forwarded_arg = ap_get_arg_at_index(parser, i);
        if (!js_loader__append_arg(arg, sizeof(arg), &arg_len, forwarded_arg)) {
            ap_free(parser);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    }
    ap_free(parser);

    bruce_process_snapshot_t snapshot;
    bruce_launch_mode_t mode = BRUCE_LAUNCH_FOREGROUND;
    if (process__snapshot(process__current_id(), &snapshot) == BRUCE_OK) {
        mode = snapshot.state == BRUCE_PROCESS_BACKGROUND ? BRUCE_LAUNCH_BACKGROUND : BRUCE_LAUNCH_FOREGROUND;
    }
    return js_loader__run_path(path, arg[0] != '\0' ? arg : NULL, mode, NULL, 0);
}
