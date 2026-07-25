#include "wifi_js.h"

#include "core_sdk/http.h"
#include "core_sdk/memory.h"
#include "core_sdk/wifi.h"

#include "native_helpers_js.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_JS_MAX_HEADERS 32

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

    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "httpFetch(url:string, options?:object|headers?:array)");
    }

    JSCStringBuf url_buf;
    const char *url = JS_ToCString(ctx, argv[0], &url_buf);
    if (url == NULL) {
        return JS_ThrowTypeError(ctx, "httpFetch(url:string, options?:object|headers?:array)");
    }

    const char *method = "GET";
    const char *body = NULL;
    size_t body_len = 0;
    const char *headers[WIFI_JS_MAX_HEADERS * 2];
    size_t header_count = 0;
    uint8_t return_response_type = 0; /* 0 = string, 1 = binary, 2 = json */

    JSCStringBuf body_buf;
    memset(&body_buf, 0, sizeof(body_buf));

    if (argc > 1) {
        if (JS_GetClassID(ctx, argv[1]) == JS_CLASS_ARRAY) {
            /* Simple array of header pairs. */
            uint32_t array_len = js_native_array_length(ctx, argv[1]);
            for (uint32_t i = 0; i + 1 < array_len && header_count < WIFI_JS_MAX_HEADERS; i += 2) {
                JSCStringBuf key_buf;
                JSCStringBuf value_buf;
                JSValue key_val = JS_GetPropertyUint32(ctx, argv[1], i);
                JSValue value_val = JS_GetPropertyUint32(ctx, argv[1], i + 1);
                const char *key = JS_IsString(ctx, key_val) ? JS_ToCString(ctx, key_val, &key_buf) : NULL;
                const char *value = JS_IsString(ctx, value_val) ? JS_ToCString(ctx, value_val, &value_buf) : NULL;
                if (key != NULL && value != NULL) {
                    headers[header_count * 2] = key;
                    headers[header_count * 2 + 1] = value;
                    header_count++;
                }
            }
        } else if (JS_IsObject(ctx, argv[1])) {
            /* Options object. */
            JSValue jsv_method = JS_GetPropertyStr(ctx, argv[1], "method");
            if (!JS_IsUndefined(jsv_method) && JS_IsString(ctx, jsv_method)) {
                JSCStringBuf method_buf;
                method = JS_ToCString(ctx, jsv_method, &method_buf);
            }

            JSValue jsv_body = JS_GetPropertyStr(ctx, argv[1], "body");
            if (!JS_IsUndefined(jsv_body)) {
                if (JS_IsString(ctx, jsv_body) || JS_IsNumber(ctx, jsv_body) || JS_IsBool(jsv_body)) {
                    body = JS_ToCString(ctx, jsv_body, &body_buf);
                    if (body != NULL) body_len = strlen(body);
                } else if (JS_IsObject(ctx, jsv_body)) {
                    JSValue global = JS_GetGlobalObject(ctx);
                    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
                    JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");
                    if (JS_IsFunction(ctx, stringify)) {
                        JS_PushArg(ctx, jsv_body);
                        JS_PushArg(ctx, stringify);
                        JS_PushArg(ctx, json);
                        JSValue jsv_body_str = JS_Call(ctx, 1);
                        if (!JS_IsException(jsv_body_str) && JS_IsString(ctx, jsv_body_str)) {
                            body = JS_ToCString(ctx, jsv_body_str, &body_buf);
                            if (body != NULL) body_len = strlen(body);
                        }
                    }
                }
            }

            JSValue jsv_response_type = JS_GetPropertyStr(ctx, argv[1], "responseType");
            if (!JS_IsUndefined(jsv_response_type) && JS_IsString(ctx, jsv_response_type)) {
                JSCStringBuf rt_buf;
                const char *response_type = JS_ToCString(ctx, jsv_response_type, &rt_buf);
                if (response_type != NULL) {
                    if (strcmp(response_type, "binary") == 0) {
                        return_response_type = 1;
                    } else if (strcmp(response_type, "json") == 0) {
                        return_response_type = 2;
                    }
                }
            }

            JSValue jsv_headers = JS_GetPropertyStr(ctx, argv[1], "headers");
            if (!JS_IsUndefined(jsv_headers)) {
                if (JS_GetClassID(ctx, jsv_headers) == JS_CLASS_ARRAY) {
                    uint32_t array_len = js_native_array_length(ctx, jsv_headers);
                    for (uint32_t i = 0; i + 1 < array_len && header_count < WIFI_JS_MAX_HEADERS; i += 2) {
                        JSCStringBuf key_buf;
                        JSCStringBuf value_buf;
                        JSValue key_val = JS_GetPropertyUint32(ctx, jsv_headers, i);
                        JSValue value_val = JS_GetPropertyUint32(ctx, jsv_headers, i + 1);
                        const char *key = JS_IsString(ctx, key_val) ? JS_ToCString(ctx, key_val, &key_buf) : NULL;
                        const char *value = JS_IsString(ctx, value_val) ? JS_ToCString(ctx, value_val, &value_buf) : NULL;
                        if (key != NULL && value != NULL) {
                            headers[header_count * 2] = key;
                            headers[header_count * 2 + 1] = value;
                            header_count++;
                        }
                    }
                } else if (JS_IsObject(ctx, jsv_headers)) {
                    uint32_t prop_count = 0;
                    for (uint32_t index = 0; header_count < WIFI_JS_MAX_HEADERS; ++index) {
                        const char *key = JS_GetOwnPropertyByIndex(ctx, index, &prop_count, jsv_headers);
                        if (key == NULL) break;
                        JSValue value_val = JS_GetPropertyStr(ctx, jsv_headers, key);
                        if (!JS_IsUndefined(value_val) &&
                            (JS_IsString(ctx, value_val) || JS_IsNumber(ctx, value_val) || JS_IsBool(value_val))) {
                            JSCStringBuf value_buf;
                            const char *value = JS_ToCString(ctx, value_val, &value_buf);
                            if (value != NULL) {
                                headers[header_count * 2] = key;
                                headers[header_count * 2 + 1] = value;
                                header_count++;
                            }
                        }
                    }
                }
            }
        }
    }

    bruce_http_request_t request = {
        .url = url,
        .method = method,
        .body = body,
        .body_len = body_len,
        .headers = headers,
        .header_count = header_count,
        .timeout_ms = 0,
    };

    bruce_http_response_t http_response = {0};
    bruce_result_t result = http__request(&request, &http_response);
    if (result != BRUCE_OK) {
        return JS_ThrowInternalError(ctx, "httpFetch failed: %d", (int)result);
    }

    JSValue headers_obj = JS_NewObject(ctx);
    for (size_t i = 0; i < http_response.header_count; ++i) {
        JS_SetPropertyStr(ctx, headers_obj, http_response.header_names[i],
                          JS_NewString(ctx, http_response.header_values[i]));
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "length", JS_NewInt32(ctx, (int)http_response.body_len));
    JS_SetPropertyStr(ctx, obj, "headers", headers_obj);
    JS_SetPropertyStr(ctx, obj, "response", JS_NewInt32(ctx, http_response.status_code));
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, http_response.status_code));
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(http_response.status_code >= 200 && http_response.status_code < 300));

    if (return_response_type == 1 && http_response.body_len > 0) {
        JS_SetPropertyStr(ctx, obj, "body",
                          JS_NewUint8ArrayCopy(ctx, (const uint8_t *)http_response.body, http_response.body_len));
    } else if (return_response_type == 2 && http_response.body_len > 0) {
        JSValue json_body = JS_Eval(ctx, http_response.body, http_response.body_len, "<http>", JS_EVAL_JSON);
        if (JS_IsException(json_body)) {
            JSValue ex = JS_GetException(ctx);
            JS_PrintValueF(ctx, ex, JS_DUMP_LONG);
            JS_SetPropertyStr(ctx, obj, "body", JS_NewStringLen(ctx, http_response.body, http_response.body_len));
        } else {
            JS_SetPropertyStr(ctx, obj, "body", json_body);
        }
    } else {
        JS_SetPropertyStr(ctx, obj, "body", JS_NewStringLen(ctx, http_response.body, http_response.body_len));
    }

    http__response_free(&http_response);
    return obj;
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
