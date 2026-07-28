#include "icon_js.h"

#include "core_sdk/icon.h"

#include "native_helpers_js.h"

JSValue native_iconGet(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    JSCStringBuf buf;
    const char *name = js_native_arg_string(ctx, argc, argv, 0, &buf);
    if (name == NULL) { return JS_NULL; }
    const char *path = icon__get(name);
    return path != NULL ? JS_NewString(ctx, path) : JS_NULL;
}
