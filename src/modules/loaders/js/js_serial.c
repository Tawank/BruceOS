#include "js_bindings_private.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"

static JSValue jsb_serial_print(JSContext *ctx, JSValue *args_ptr)
{
    jsb_print_args(ctx, args_ptr, false);
    return JS_UNDEFINED;
}

static JSValue jsb_serial_println(JSContext *ctx, JSValue *args_ptr)
{
    jsb_print_args(ctx, args_ptr, true);
    return JS_UNDEFINED;
}

static JSValue jsb_serial_readln(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    char line[256];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        line[0] = '\0';
    } else {
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
    }
    return JS_NewString(ctx, line);
}

static JSValue jsb_serial_cmd(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf sb;
    const char *line = jsb_arg_string(ctx, args_ptr, 0, &sb);
    if (line == NULL || line[0] == '\0') {
        return JS_NewBool(false);
    }

    /* Minimal terminal-parser duplication using only public Core APIs.
     * Splits the first whitespace-delimited token and forwards the rest
     * to app_runner__run() or app_runner__run_path(), matching terminal.c. */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char token[256];
    size_t ti = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && ti + 1 < sizeof(token)) {
        token[ti++] = *p++;
    }
    token[ti] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    const char *arg = (*p != '\0') ? p : NULL;

    if (token[0] == '/' || strncmp(token, "./", 2) == 0) {
        return JS_NewBool(app_runner__run_path(token, arg, false) > 0);
    }
    return JS_NewBool(app_runner__run(token, arg, false) > 0);
}

static const js_binding_t s_serial_bindings[] = {
    {"serial.print", jsb_serial_print},
    {"serial.println", jsb_serial_println},
    {"serial.readln", jsb_serial_readln},
    {"serial.cmd", jsb_serial_cmd},
};

static const char s_serial_setup[] =
    "(function(){"
    "var bridge=globalThis.__bruce_bridge;"
    "if(typeof bridge!=='function')return;"
    "globalThis.serial=globalThis.serial||{};"
    "var methods=['print','println','readln','cmd'];"
    "for(var i=0;i<methods.length;i++){"
    "(function(full,method){"
    "globalThis.serial[method]=function(){return bridge(full,Array.prototype.slice.call(arguments));};"
    "})('serial.'+methods[i],methods[i]);"
    "}"
    "})();";

void jsb_serial__init(JSContext *ctx)
{
    js_bindings__add_module(s_serial_bindings,
                            sizeof(s_serial_bindings) / sizeof(s_serial_bindings[0]));
    JSValue result = JS_Eval(ctx, s_serial_setup, sizeof(s_serial_setup) - 1, "js_serial_setup", 0);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        printf("[js_serial] setup error: ");
        JS_PrintValueF(ctx, ex, JS_DUMP_LONG);
        printf("\n");
    }
}
