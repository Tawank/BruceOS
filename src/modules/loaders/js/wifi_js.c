#include "wifi_js.h"

#include "core_sdk/wifi.h"

static const char *wifi_enc_types[] = {
    "OPEN",
    "WEP",
    "WPA_PSK",
    "WPA2_PSK",
    "WPA_WPA2_PSK",
    "ENTERPRISE",
    "WPA2_ENTERPRISE",
    "WPA3_PSK",
    "WPA2_WPA3_PSK",
    "WAPI_PSK",
    "WPA3_ENT_192",
    "MAX"
};

JSValue native_wifiConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(wifi__is_connected());
}

JSValue native_wifiConnectDialog(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    /* No Core dialog-driven Wi-Fi connection API; use wifi.connect() instead. */
    return JS_NewBool(false);
}

JSValue native_wifiConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "wifi.connect(ssid:string, timeout?:int, pwd?:string)");
    }

    JSCStringBuf ssb;
    const char *ssid = JS_ToCString(ctx, argv[0], &ssb);

    int timeout_sec = 10;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) {
        JS_ToInt32(ctx, &timeout_sec, argv[1]);
    }
    if (timeout_sec < 1) {
        timeout_sec = 1;
    }

    const char *pwd = NULL;
    JSCStringBuf psb;
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        pwd = JS_ToCString(ctx, argv[2], &psb);
    }

    bruce_result_t result = wifi__connect(ssid, pwd, (uint32_t)timeout_sec * 1000u);
    return JS_NewBool(result == BRUCE_OK);
}

JSValue native_wifiScan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;

    wifi__network_t networks[32];
    int count = wifi__scan(networks, sizeof(networks) / sizeof(networks[0]));
    if (count < 0) {
        return JS_ThrowInternalError(ctx, "wifi.scan failed: %d", count);
    }

    JSValue arr = JS_NewArray(ctx, count);
    if (JS_IsException(arr)) {
        return arr;
    }

    for (int i = 0; i < count; i++) {
        JSValue obj = JS_NewObject(ctx);
        int enctypeInt = (int)networks[i].authmode;
        const char *enctype = enctypeInt < 12 ? wifi_enc_types[enctypeInt] : "UNKNOWN";
        JS_SetPropertyStr(ctx, obj, "encryptionType", JS_NewString(ctx, enctype));
        JS_SetPropertyStr(ctx, obj, "SSID", JS_NewString(ctx, networks[i].ssid));
        JS_SetPropertyStr(ctx, obj, "MAC", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, obj, "RSSI", JS_NewInt32(ctx, networks[i].rssi));
        JS_SetPropertyStr(ctx, obj, "channel", JS_NewInt32(ctx, networks[i].channel));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
    }
    return arr;
}

JSValue native_wifiDisconnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int)wifi__disconnect());
}

JSValue native_httpFetch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_ThrowInternalError(ctx, "wifi.httpFetch is not implemented in this Core ABI version");
}

JSValue native_wifiMACAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    char *mac = wifi__get_mac();
    return JS_NewString(ctx, mac != NULL ? mac : "");
}

JSValue native_ipAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    char *ip = wifi__get_ip();
    if (ip != NULL) {
        return JS_NewString(ctx, ip);
    }
    return JS_NULL;
}
