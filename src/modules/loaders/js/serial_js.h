#pragma once

#include "mquickjs.h"

JSValue native_serialPrint(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_serialPrintln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_serialReadln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_serialCmd(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
