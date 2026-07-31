#include "serial_js.h"

#include "core_sdk/stdio.h"
#include "modules/utils/serial_commands/serial_commands_app.h"
#include "native_helpers_js.h"

#include <string.h>

JSValue native_serialPrint(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    js_native_print(ctx, argc, argv, false);
    return JS_UNDEFINED;
}

JSValue native_serialPrintln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    js_native_print(ctx, argc, argv, true);
    return JS_UNDEFINED;
}

JSValue native_serialReadln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    char line[256];
    if (stdio__read_line(line, sizeof(line), false) < 0) { line[0] = '\0'; }
    return JS_NewString(ctx, line);
}

JSValue native_serialCmd(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    JSCStringBuf sb;
    const char *line = js_native_arg_string(ctx, argc, argv, 0, &sb);
    if (line == NULL || line[0] == '\0') { return JS_NewBool(false); }

    return JS_NewBool(serial_commands__run_line(line, false) > 0);
}
