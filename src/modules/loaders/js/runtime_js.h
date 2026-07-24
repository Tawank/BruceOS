#pragma once

#include "mquickjs.h"

JSValue native_runtimeToBackground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_runtimeToForeground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_runtimeIsForeground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_runtimeMain(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
