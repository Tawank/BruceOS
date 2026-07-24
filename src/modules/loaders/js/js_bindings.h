#pragma once

/* JavaScript-to-Core SDK binding bridge for the built-in JS loader module.
 * The mquickjs stdlib exposes a single C function `__bruce_bridge(name, args)`;
 * this file defines the user-facing namespaces (runtime, serial, wifi, display,
 * dialog, ...) and dispatches each call to the appropriate public Core API.
 *
 * This code lives in the src/ component (not in components/mquickjs) so it can
 * include core_sdk/... headers and call Core APIs without a component circular
 * dependency. */

#include "mquickjs.h"

/* Register the Bruce API namespaces on `ctx`'s global object.
 * Must be called once after JS_NewContext() and before JS_Eval(). */
void js_bindings__register(JSContext *ctx);

/* C bridge invoked by the mquickjs stdlib function `__bruce_bridge`.
 * `name` is the fully-qualified binding name (e.g. "wifi.scan").
 * `args_array` is a JS Array (or undefined) holding the caller's arguments. */
JSValue js_bindings__dispatch(JSContext *ctx, const char *name, JSValue args_array);

/* Per-module initializers called by js_bindings__register(). */
void jsb_runtime__init(JSContext *ctx);
void jsb_serial__init(JSContext *ctx);
void jsb_wifi__init(JSContext *ctx);
void jsb_display__init(JSContext *ctx);
void jsb_dialog__init(JSContext *ctx);
