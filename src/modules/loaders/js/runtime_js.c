#include "runtime_js.h"

#include "core_sdk/process.h"

JSValue native_runtimeToBackground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int)process__to_background());
}

JSValue native_runtimeToForeground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int)process__to_foreground());
}

JSValue native_runtimeIsForeground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    bruce_process_snapshot_t snapshot;
    bool fg = false;
    if (process__snapshot(process__current_id(), &snapshot) == BRUCE_OK) {
        fg = (snapshot.state == BRUCE_PROCESS_FOREGROUND);
    }
    return JS_NewBool(fg);
}

JSValue native_runtimeMain(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    /* runtime.main() is retained for backward compatibility but the normal
     * JS lifecycle entry is app_main(argv).  The callback is not scheduled
     * here; scripts should use app_main instead. */
    return JS_UNDEFINED;
}
