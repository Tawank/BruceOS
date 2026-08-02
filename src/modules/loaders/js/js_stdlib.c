#include "js_stdlib.h"
#include "js_source.h"
#include "native_helpers_js.h"

#include "core_sdk/runtime.h"

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

    js_source_t source = {0};
    bruce_result_t result = js_source__load(path, JS_STDLIB_LOAD_MAX_SIZE, &source);
    if (result != BRUCE_OK) {
        if (result == BRUCE_ERR_NO_MEMORY || result == BRUCE_ERR_RESOURCE_LIMIT) {
            return JS_ThrowOutOfMemory(ctx);
        }
        return JS_ThrowReferenceError(ctx, "load: error reading %s", path);
    }
    JSValue eval_result = JS_Eval(ctx, (const char *)source.data, source.size, path, 0);
    js_source__release(&source);
    return eval_result;
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
