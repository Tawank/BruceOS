#include "notification_js.h"

#include <string.h>

#include "core_sdk/notification.h"
#include "core_sdk/status_icon.h"

JSValue native_notificationPush(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsString(ctx, argv[0]) || !JS_IsNumber(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "notification.push(text:string, durationMs:int)");
    }
    JSCStringBuf text_buf;
    const char *text = JS_ToCString(ctx, argv[0], &text_buf);
    int duration = 0;
    JS_ToInt32(ctx, &duration, argv[1]);
    if (text == NULL || duration < 0) {
        return JS_ThrowTypeError(ctx, "notification.push(text:string, durationMs:int)");
    }
    bruce_result_t result = notification__push(text, (uint32_t)duration);
    return JS_NewInt32(ctx, (int32_t)result);
}

JSValue native_notificationDismiss(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int32_t)notification__dismiss());
}

JSValue native_statusIconPush(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 4 || !JS_IsString(ctx, argv[0]) || JS_GetClassID(ctx, argv[1]) != JS_CLASS_UINT8_ARRAY ||
        !JS_IsNumber(ctx, argv[2]) || !JS_IsNumber(ctx, argv[3])) {
        return JS_ThrowTypeError(
            ctx, "statusIcon.push(key:string, bitmap:Uint8Array, width:int, height:int)"
        );
    }
    JSCStringBuf key_buf;
    const char *key = JS_ToCString(ctx, argv[0], &key_buf);
    int width = 0;
    int height = 0;
    JS_ToInt32(ctx, &width, argv[2]);
    JS_ToInt32(ctx, &height, argv[3]);
    size_t length = 0;
    const uint8_t *bitmap = (const uint8_t *)JS_GetTypedArrayBuffer(ctx, &length, argv[1]);
    if (key == NULL || width <= 0 || height <= 0 || width > UINT8_MAX || height > UINT8_MAX ||
        length < (size_t)((width + 7) / 8) * (size_t)height) {
        return JS_ThrowTypeError(ctx, "statusIcon bitmap is shorter than its dimensions");
    }
    return JS_NewInt32(ctx, (int32_t)status_icon__push(key, bitmap, (uint8_t)width, (uint8_t)height));
}

JSValue native_statusIconRemove(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "statusIcon.remove(key:string)");
    }
    JSCStringBuf key_buf;
    const char *key = JS_ToCString(ctx, argv[0], &key_buf);
    return JS_NewInt32(ctx, (int32_t)status_icon__remove(key));
}

JSValue native_statusIconList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    bruce_status_icon_t icons[BRUCE_STATUS_ICON_MAX];
    size_t count = 0;
    uint32_t revision = 0;
    bruce_result_t result = status_icon__list(icons, BRUCE_STATUS_ICON_MAX, &count, &revision);
    if (result != BRUCE_OK) { return JS_ThrowInternalError(ctx, "statusIcon.list failed: %d", (int)result); }
    JSValue array = JS_NewArray(ctx, (int)count);
    JS_SetPropertyStr(ctx, array, "revision", JS_NewInt32(ctx, (int32_t)revision));
    for (size_t i = 0; i < count; ++i) {
        size_t bitmap_size = (size_t)((icons[i].width + 7) / 8) * icons[i].height;
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "key", JS_NewString(ctx, icons[i].key));
        JS_SetPropertyStr(ctx, item, "width", JS_NewInt32(ctx, icons[i].width));
        JS_SetPropertyStr(ctx, item, "height", JS_NewInt32(ctx, icons[i].height));
        JS_SetPropertyStr(ctx, item, "bitmap", JS_NewUint8ArrayCopy(ctx, icons[i].bitmap, bitmap_size));
        JS_SetPropertyUint32(ctx, array, (uint32_t)i, item);
    }
    return array;
}
