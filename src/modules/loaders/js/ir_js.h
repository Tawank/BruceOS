#pragma once

#include "mquickjs.h"

JSValue native_irRead(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irReadRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irTransmitFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irTransmitRecord(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irTransmit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
