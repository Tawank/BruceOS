#pragma once

#include "mquickjs.h"

JSValue native_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_num_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_hex_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getKeysPressed(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getPrevPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getSelPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getEscPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getNextPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getAnyPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_setLongPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
