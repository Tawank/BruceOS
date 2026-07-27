#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mquickjs.h"

/* TypedArray check matching the legacy helpers_js helper. */
static inline bool JS_IsTypedArray(JSContext *ctx, JSValue val) {
    int class_id = JS_GetClassID(ctx, val);
    return (class_id >= JS_CLASS_ARRAY_BUFFER && class_id <= JS_CLASS_UINT32_ARRAY);
}

/* Argument parsing helpers (argc/argv style used by mquickjs C functions). */
static inline int js_native_arg_int(JSContext *ctx, int argc, JSValue *argv, int idx, int def) {
    if (idx >= argc || !JS_IsNumber(ctx, argv[idx])) { return def; }
    int v = def;
    JS_ToInt32(ctx, &v, argv[idx]);
    return v;
}

static inline bool js_native_arg_bool(JSContext *ctx, int argc, JSValue *argv, int idx, bool def) {
    if (idx >= argc || !JS_IsBool(argv[idx])) { return def; }
    return JS_ToBool(ctx, argv[idx]) != 0;
}

static inline const char *
js_native_arg_string(JSContext *ctx, int argc, JSValue *argv, int idx, JSCStringBuf *buf) {
    if (idx >= argc || !JS_IsString(ctx, argv[idx])) { return NULL; }
    return JS_ToCString(ctx, argv[idx], buf);
}

static inline bool js_native_arg_is_object(JSContext *ctx, int argc, JSValue *argv, int idx) {
    if (idx >= argc) { return false; }
    return JS_IsObject(ctx, argv[idx]);
}

static inline bool js_native_arg_is_array(JSContext *ctx, int argc, JSValue *argv, int idx) {
    if (idx >= argc) { return false; }
    return JS_IsObject(ctx, argv[idx]) && JS_GetClassID(ctx, argv[idx]) == JS_CLASS_ARRAY;
}

static inline uint32_t js_native_array_length(JSContext *ctx, JSValue arr) {
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    if (JS_IsNumber(ctx, len)) { JS_ToUint32(ctx, &n, len); }
    return n;
}

/* Print helpers for modules that forward JS values to stdout (legacy serial/terminal behaviour). */
static inline void js_native_print_value(JSContext *ctx, JSValue v) {
    if (JS_IsUndefined(v)) {
        printf("undefined");
    } else if (JS_IsNull(v)) {
        printf("null");
    } else if (JS_IsNumber(ctx, v)) {
        double num = 0.0;
        JS_ToNumber(ctx, &num, v);
        printf("%g", num);
    } else if (JS_IsBool(v)) {
        printf("%s", JS_ToBool(ctx, v) ? "true" : "false");
    } else if (JS_IsString(ctx, v)) {
        JSCStringBuf sb;
        const char *s = JS_ToCString(ctx, v, &sb);
        if (s != NULL) { printf("%s", s); }
    } else {
        JS_PrintValueF(ctx, v, JS_DUMP_LONG);
    }
}

static inline void js_native_print(JSContext *ctx, int argc, JSValue *argv, bool newline) {
    for (int i = 0; i < argc && i < 20; i++) {
        if (i > 0) { printf(" "); }
        js_native_print_value(ctx, argv[i]);
    }
    if (newline) { printf("\n"); }
}
