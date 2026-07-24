#ifndef JS_BINDINGS_PRIVATE_H
#define JS_BINDINGS_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "js_port.h"
#include "mquickjs.h"

/* Type of a C function that implements a Bruce JS binding. */
typedef JSValue (*js_binding_fn_t)(JSContext *ctx, JSValue *args_ptr);

/* Single binding entry: "namespace.method" -> C implementation. */
typedef struct {
    const char *name;
    js_binding_fn_t fn;
} js_binding_t;

/* Argument helpers shared by all JS binding modules. */
JSValue jsb_get_arg(JSContext *ctx, JSValue *args_ptr, uint32_t idx);
bool jsb_arg_int(JSContext *ctx, JSValue *args_ptr, uint32_t idx, int *out);
bool jsb_arg_bool(JSContext *ctx, JSValue *args_ptr, uint32_t idx, int *out);
const char *jsb_arg_string(JSContext *ctx, JSValue *args_ptr, uint32_t idx, JSCStringBuf *buf);
bool jsb_arg_is_object(JSContext *ctx, JSValue *args_ptr, uint32_t idx);
bool jsb_arg_is_array(JSContext *ctx, JSValue *args_ptr, uint32_t idx);
uint32_t jsb_array_length(JSContext *ctx, JSValue arr);
void jsb_print_values(JSContext *ctx, int argc, JSValue *argv, bool newline);
void jsb_print_args(JSContext *ctx, JSValue *args_ptr, bool newline);

/* Central dispatcher registration. Each module calls this from its __init(). */
void js_bindings__add_module(const js_binding_t *bindings, size_t count);

#endif /* JS_BINDINGS_PRIVATE_H */
