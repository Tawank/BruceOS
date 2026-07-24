#include "js_bindings_private.h"

#include "core_sdk/wifi.h"

static JSValue jsb_wifi_connected(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewBool(wifi__is_connected());
}

static JSValue jsb_wifi_connectDialog(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    /* No Core dialog-driven Wi-Fi connection API; use wifi.connect() instead. */
    return JS_NewBool(false);
}

static JSValue jsb_wifi_connect(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf ssid_buf;
    const char *ssid = jsb_arg_string(ctx, args_ptr, 0, &ssid_buf);
    if (ssid == NULL) {
        return JS_ThrowTypeError(ctx, "wifi.connect(ssid, timeout?, pwd?)");
    }
    int timeout_sec = 10;
    jsb_arg_int(ctx, args_ptr, 1, &timeout_sec);
    if (timeout_sec < 1) {
        timeout_sec = 1;
    }
    JSCStringBuf pwd_buf;
    const char *pwd = jsb_arg_string(ctx, args_ptr, 2, &pwd_buf);
    return JS_NewBool(wifi__connect(ssid, pwd, (uint32_t)timeout_sec * 1000u) == BRUCE_OK);
}

static JSValue jsb_wifi_scan(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    wifi__network_t networks[32];
    int count = wifi__scan(networks, sizeof(networks) / sizeof(networks[0]));
    if (count < 0) {
        return JS_ThrowInternalError(ctx, "wifi.scan failed: %d", count);
    }
    JSValue arr = JS_NewArray(ctx, count);
    if (JS_IsException(arr)) {
        return arr;
    }
    for (int i = 0; i < count; ++i) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "SSID", JS_NewString(ctx, networks[i].ssid));
        JS_SetPropertyStr(ctx, obj, "RSSI", JS_NewInt32(ctx, networks[i].rssi));
        JS_SetPropertyStr(ctx, obj, "channel", JS_NewInt32(ctx, networks[i].channel));
        JS_SetPropertyStr(ctx, obj, "encryptionType", JS_NewInt32(ctx, networks[i].authmode));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
    }
    return arr;
}

static JSValue jsb_wifi_disconnect(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewInt32(ctx, (int)wifi__disconnect());
}

static JSValue jsb_wifi_httpFetch(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_ThrowInternalError(ctx, "wifi.httpFetch is not implemented in this Core ABI version");
}

static JSValue jsb_wifi_macAddress(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    char *mac = wifi__get_mac();
    return JS_NewString(ctx, mac != NULL ? mac : "");
}

static JSValue jsb_wifi_ipAddress(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    char *ip = wifi__get_ip();
    return JS_NewString(ctx, ip != NULL ? ip : "");
}

static const js_binding_t s_wifi_bindings[] = {
    {"wifi.connected", jsb_wifi_connected},
    {"wifi.connectDialog", jsb_wifi_connectDialog},
    {"wifi.connect", jsb_wifi_connect},
    {"wifi.scan", jsb_wifi_scan},
    {"wifi.disconnect", jsb_wifi_disconnect},
    {"wifi.httpFetch", jsb_wifi_httpFetch},
    {"wifi.macAddress", jsb_wifi_macAddress},
    {"wifi.ipAddress", jsb_wifi_ipAddress},
};

static const char s_wifi_setup[] =
    "(function(){"
    "var bridge=globalThis.__bruce_bridge;"
    "if(typeof bridge!=='function')return;"
    "globalThis.wifi=globalThis.wifi||{};"
    "var methods=['connected','connectDialog','connect','scan','disconnect','httpFetch','macAddress','ipAddress'];"
    "for(var i=0;i<methods.length;i++){"
    "(function(full,method){"
    "globalThis.wifi[method]=function(){return bridge(full,Array.prototype.slice.call(arguments));};"
    "})('wifi.'+methods[i],methods[i]);"
    "}"
    "})();";

void jsb_wifi__init(JSContext *ctx)
{
    js_bindings__add_module(s_wifi_bindings,
                            sizeof(s_wifi_bindings) / sizeof(s_wifi_bindings[0]));
    JSValue result = JS_Eval(ctx, s_wifi_setup, sizeof(s_wifi_setup) - 1, "js_wifi_setup", 0);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        printf("[js_wifi] setup error: ");
        JS_PrintValueF(ctx, ex, JS_DUMP_LONG);
        printf("\n");
    }
}
