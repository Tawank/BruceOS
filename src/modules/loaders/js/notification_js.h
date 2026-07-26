#pragma once

#include "mquickjs.h"

JSValue native_notificationPush(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_notificationDismiss(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_statusIconPush(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_statusIconRemove(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_statusIconList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
