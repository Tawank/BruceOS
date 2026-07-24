#pragma once

#include "native_helpers_js.h"

JSValue native_wifiConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiConnectDialog(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiScan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiDisconnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_httpFetch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiMACAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_ipAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
