#include "serial_js.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"

#include <stdio.h>

JSValue native_serialPrint(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    js_native_print(ctx, argc, argv, false);
    return JS_UNDEFINED;
}

JSValue native_serialPrintln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    js_native_print(ctx, argc, argv, true);
    return JS_UNDEFINED;
}

JSValue native_serialReadln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
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

JSValue native_serialCmd(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    JSCStringBuf sb;
    const char *line = js_native_arg_string(ctx, argc, argv, 0, &sb);
    if (line == NULL || line[0] == '\0') {
        return JS_NewBool(false);
    }

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
