#pragma once

#include "mquickjs.h"

/* Standard-library C closures referenced by the generated mquickjs stdlib
 * table (mqjs_stdlib.h).  These are implemented in js_stdlib.c and used by
 * the JS loader. */

JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_load(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_clearTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_require(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_delay(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_random(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
