#include "js_bindings_private.h"

#include "core_sdk/task.h"

static JSValue jsb_runtime_toBackground(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewInt32(ctx, (int)task__to_background());
}

static JSValue jsb_runtime_toForeground(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    bruce_task_id_t id = task__current_id();
    return JS_NewInt32(ctx, (int)task__foreground(id));
}

static JSValue jsb_runtime_isForeground(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    bruce_task_snapshot_t snapshot;
    bool fg = false;
    if (task__snapshot(task__current_id(), &snapshot) == BRUCE_OK) {
        fg = (snapshot.state == BRUCE_TASK_FOREGROUND);
    }
    return JS_NewBool(fg);
}

static JSValue jsb_runtime_sleep(JSContext *ctx, JSValue *args_ptr)
{
    int ms = 0;
    if (!jsb_arg_int(ctx, args_ptr, 0, &ms) || ms < 0) {
        ms = 0;
    }
    return JS_NewInt32(ctx, (int)runtime__sleep((uint32_t)ms));
}

static JSValue jsb_runtime_delay(JSContext *ctx, JSValue *args_ptr)
{
    int ms = 0;
    if (!jsb_arg_int(ctx, args_ptr, 0, &ms) || ms < 0) {
        ms = 0;
    }
    return JS_NewInt32(ctx, (int)runtime__delay((uint32_t)ms));
}

static JSValue jsb_runtime_main(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    /* runtime.main() is retained for backward compatibility but the normal
     * JS lifecycle entry is app_main(argv).  The callback is not scheduled
     * here; scripts should use app_main instead. */
    return JS_UNDEFINED;
}

static const js_binding_t s_runtime_bindings[] = {
    {"runtime.toBackground", jsb_runtime_toBackground},
    {"runtime.toForeground", jsb_runtime_toForeground},
    {"runtime.isForeground", jsb_runtime_isForeground},
    {"runtime.sleep", jsb_runtime_sleep},
    {"runtime.delay", jsb_runtime_delay},
    {"runtime.main", jsb_runtime_main},
};

static const char s_runtime_setup[] =
    "(function(){"
    "var bridge=globalThis.__bruce_bridge;"
    "if(typeof bridge!=='function')return;"
    "globalThis.runtime=globalThis.runtime||{};"
    "var methods=['toBackground','toForeground','isForeground','sleep','delay','main'];"
    "for(var i=0;i<methods.length;i++){"
    "(function(full,method){"
    "globalThis.runtime[method]=function(){return bridge(full,Array.prototype.slice.call(arguments));};"
    "})('runtime.'+methods[i],methods[i]);"
    "}"
    "})();";

void jsb_runtime__init(JSContext *ctx)
{
    js_bindings__add_module(s_runtime_bindings,
                            sizeof(s_runtime_bindings) / sizeof(s_runtime_bindings[0]));
    JSValue result = JS_Eval(ctx, s_runtime_setup, sizeof(s_runtime_setup) - 1, "js_runtime_setup", 0);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        printf("[js_runtime] setup error: ");
        JS_PrintValueF(ctx, ex, JS_DUMP_LONG);
        printf("\n");
    }
}
