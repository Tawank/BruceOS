#include "js_stdlib.h"
#include "native_helpers_js.h"

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"
#include "core_sdk/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define JS_STDLIB_LOAD_MAX_SIZE (64 * 1024u)

JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    return JS_NewInt64(ctx, (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;

    return JS_NewInt64(ctx, (int64_t)runtime__now());
}

JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;

    js_native_print(ctx, argc, argv, true);
    return JS_UNDEFINED;
}

JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;

    JS_GC(ctx);
    return JS_UNDEFINED;
}

JSValue js_load(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;

    if (argc < 1 || !JS_IsString(ctx, argv[0])) { return JS_ThrowTypeError(ctx, "load(path:string)"); }

    JSCStringBuf path_buf;
    const char *path = JS_ToCString(ctx, argv[0], &path_buf);
    if (path == NULL) { return JS_ThrowTypeError(ctx, "load(path:string)"); }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) { return JS_ThrowReferenceError(ctx, "load: cannot open %s", path); }

    int result = BRUCE_OK;
    uint64_t size = 0;
    if (storage__seek(file, 0, SEEK_END, &size) != BRUCE_OK || size == 0 || size > JS_STDLIB_LOAD_MAX_SIZE) {
        result = BRUCE_ERR_IO;
    } else {
        char *source = memory__malloc((size_t)size + 1);
        if (source == NULL) {
            result = BRUCE_ERR_NO_MEMORY;
        } else {
            if (storage__seek(file, 0, SEEK_SET, NULL) != BRUCE_OK) {
                result = BRUCE_ERR_IO;
            } else {
                size_t read_size = 0;
                if (storage__read(file, source, (size_t)size, &read_size) != BRUCE_OK ||
                    read_size != (size_t)size) {
                    result = BRUCE_ERR_IO;
                } else {
                    source[size] = '\0';
                    JSValue eval_result = JS_Eval(ctx, source, (size_t)size, path, 0);
                    memory__free(source);
                    storage__close(file);
                    return eval_result;
                }
            }
            memory__free(source);
        }
    }

    storage__close(file);
    if (result == BRUCE_ERR_NO_MEMORY) { return JS_ThrowOutOfMemory(ctx); }
    return JS_ThrowReferenceError(ctx, "load: error reading %s", path);
}

JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;

    /* Timers are not implemented in the embedded JS runtime. */
    return JS_UNDEFINED;
}

JSValue js_clearTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;

    /* Timers are not implemented in the embedded JS runtime. */
    return JS_UNDEFINED;
}

JSValue js_require(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) { return JS_ThrowTypeError(ctx, "require(name:string)"); }

    JSCStringBuf name_buf;
    const char *name = JS_ToCString(ctx, argv[0], &name_buf);
    if (name == NULL) { return JS_ThrowTypeError(ctx, "require(name:string)"); }

    JSValue module = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), name);
    if (JS_IsUndefined(module)) { return JS_ThrowReferenceError(ctx, "module '%s' is not available", name); }
    return module;
}

JSValue js_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt64(ctx, (int64_t)runtime__now());
}

JSValue js_delay(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsNumber(ctx, argv[0])) { return JS_ThrowTypeError(ctx, "delay(milliseconds:number)"); }

    int milliseconds = 0;
    JS_ToInt32(ctx, &milliseconds, argv[0]);
    if (milliseconds > 0) { (void)runtime__delay((uint32_t)milliseconds); }
    return JS_UNDEFINED;
}

JSValue js_random(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsNumber(ctx, argv[0]) || (argc > 1 && !JS_IsNumber(ctx, argv[1]))) {
        return JS_ThrowTypeError(ctx, "random([min,] max)");
    }

    int min = 0;
    int max = 0;
    if (argc == 1) {
        JS_ToInt32(ctx, &max, argv[0]);
    } else {
        JS_ToInt32(ctx, &min, argv[0]);
        JS_ToInt32(ctx, &max, argv[1]);
    }
    if (max <= min) { return JS_NewInt32(ctx, min); }
    return JS_NewInt32(ctx, min + rand() % (max - min));
}
