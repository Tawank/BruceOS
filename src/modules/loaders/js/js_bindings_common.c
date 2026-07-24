#include "js_bindings_private.h"

JSValue jsb_get_arg(JSContext *ctx, JSValue *args_ptr, uint32_t idx)
{
    if (args_ptr == NULL) {
        return JS_UNDEFINED;
    }
    return JS_GetPropertyUint32(ctx, *args_ptr, idx);
}

bool jsb_arg_int(JSContext *ctx, JSValue *args_ptr, uint32_t idx, int *out)
{
    JSValue v = jsb_get_arg(ctx, args_ptr, idx);
    if (JS_IsUndefined(v) || !JS_IsNumber(ctx, v)) {
        return false;
    }
    return JS_ToInt32(ctx, out, v) == 0;
}

bool jsb_arg_bool(JSContext *ctx, JSValue *args_ptr, uint32_t idx, int *out)
{
    JSValue v = jsb_get_arg(ctx, args_ptr, idx);
    if (JS_IsUndefined(v)) {
        return false;
    }
    *out = JS_ToBool(ctx, v);
    return true;
}

const char *jsb_arg_string(JSContext *ctx, JSValue *args_ptr, uint32_t idx, JSCStringBuf *buf)
{
    JSValue v = jsb_get_arg(ctx, args_ptr, idx);
    if (JS_IsUndefined(v) || JS_IsNull(v) || !JS_IsString(ctx, v)) {
        return NULL;
    }
    return JS_ToCString(ctx, v, buf);
}

bool jsb_arg_is_object(JSContext *ctx, JSValue *args_ptr, uint32_t idx)
{
    JSValue v = jsb_get_arg(ctx, args_ptr, idx);
    return JS_IsObject(ctx, v);
}

bool jsb_arg_is_array(JSContext *ctx, JSValue *args_ptr, uint32_t idx)
{
    JSValue v = jsb_get_arg(ctx, args_ptr, idx);
    return JS_IsObject(ctx, v) && JS_GetClassID(ctx, v) == JS_CLASS_ARRAY;
}

uint32_t jsb_array_length(JSContext *ctx, JSValue arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    if (JS_IsNumber(ctx, len)) {
        JS_ToUint32(ctx, &n, len);
    }
    return n;
}

void jsb_print_values(JSContext *ctx, int argc, JSValue *argv, bool newline)
{
    for (int i = 0; i < argc; i++) {
        if (i != 0) {
            putchar(' ');
        }
        JSValue v = argv[i];
        if (JS_IsString(ctx, v)) {
            JSCStringBuf buf;
            size_t len = 0;
            const char *str = JS_ToCStringLen(ctx, &len, v, &buf);
            if (str != NULL) {
                fwrite(str, 1, len, stdout);
            }
        } else {
            JS_PrintValueF(ctx, v, JS_DUMP_LONG);
        }
    }
    if (newline) {
        putchar('\n');
    }
}

void jsb_print_args(JSContext *ctx, JSValue *args_ptr, bool newline)
{
    if (args_ptr == NULL) {
        if (newline) {
            putchar('\n');
        }
        return;
    }
    uint32_t len = jsb_array_length(ctx, *args_ptr);
    if (len == 0 && !newline) {
        return;
    }
    JSValue *argv = memory__malloc(sizeof(JSValue) * (len > 0 ? len : 1));
    if (argv == NULL) {
        if (newline) {
            putchar('\n');
        }
        return;
    }
    for (uint32_t i = 0; i < len; ++i) {
        argv[i] = JS_GetPropertyUint32(ctx, *args_ptr, i);
    }
    jsb_print_values(ctx, (int)len, argv, newline);
    memory__free(argv);
}
