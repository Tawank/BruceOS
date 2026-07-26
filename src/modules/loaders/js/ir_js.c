#include "ir_js.h"

#include <stdint.h>
#include <stdlib.h>

#include "core_sdk/ir.h"

#define IR_JS_CAPTURE_SIZE 8192u

static JSValue ir_js__read(JSContext *ctx, int argc, JSValue *argv, bool raw)
{
    int timeout_seconds = 10;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &timeout_seconds, argv[0]);
    if (timeout_seconds < 1 || (uint32_t)timeout_seconds > UINT32_MAX / 1000u) {
        return JS_ThrowRangeError(ctx, "ir.read timeout out of range");
    }
    char *capture = malloc(IR_JS_CAPTURE_SIZE);
    if (capture == NULL) return JS_ThrowInternalError(ctx, "ir.read out of memory");
    bruce_result_t result = ir__receive(raw, (uint32_t)timeout_seconds * 1000u, capture, IR_JS_CAPTURE_SIZE);
    JSValue value = JS_NewString(ctx, result == BRUCE_OK ? capture : "");
    free(capture);
    return value;
}

JSValue native_irRead(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    return ir_js__read(ctx, argc, argv, false);
}

JSValue native_irReadRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    return ir_js__read(ctx, argc, argv, true);
}

JSValue native_irTransmit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "ir.transmit(data:string, protocol?:string, bits?:int)");
    }
    JSCStringBuf data_buf;
    JSCStringBuf protocol_buf;
    const char *data = JS_ToCString(ctx, argv[0], &data_buf);
    const char *protocol = "NEC";
    if (argc > 1 && JS_IsString(ctx, argv[1])) protocol = JS_ToCString(ctx, argv[1], &protocol_buf);
    int bits = 32;
    if (argc > 2 && JS_IsNumber(ctx, argv[2])) JS_ToInt32(ctx, &bits, argv[2]);
    if (bits < 1 || bits > 32) return JS_ThrowRangeError(ctx, "ir.transmit bits out of range");
    bruce_result_t result = ir__transmit(data, protocol, (uint8_t)bits, 0);
    return JS_NewBool(result == BRUCE_OK);
}

JSValue native_irTransmitFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "ir.transmitFile(path:string)");
    }
    JSCStringBuf path_buf;
    const char *path = JS_ToCString(ctx, argv[0], &path_buf);
    return JS_NewBool(ir__transmit_file(path, 0) == BRUCE_OK);
}
