#include "js_bindings_private.h"

#include "core_sdk/display.h"

static JSValue jsb_display_color(JSContext *ctx, JSValue *args_ptr)
{
    int r = 0, g = 0, b = 0;
    jsb_arg_int(ctx, args_ptr, 0, &r);
    jsb_arg_int(ctx, args_ptr, 1, &g);
    jsb_arg_int(ctx, args_ptr, 2, &b);
    return JS_NewInt32(ctx, (int)display__color565((uint8_t)r, (uint8_t)g, (uint8_t)b));
}

static JSValue jsb_display_setTextColor(JSContext *ctx, JSValue *args_ptr)
{
    int c = 0;
    if (jsb_arg_int(ctx, args_ptr, 0, &c)) {
        display__set_text_color((bruce_display_color_t)c);
        int bg = -1;
        if (jsb_arg_int(ctx, args_ptr, 1, &bg) && bg >= 0) {
            display__set_text_bg_color((uint32_t)bg);
        }
    }
    return JS_UNDEFINED;
}

static JSValue jsb_display_setTextSize(JSContext *ctx, JSValue *args_ptr)
{
    int s = 1;
    jsb_arg_int(ctx, args_ptr, 0, &s);
    display__set_text_size((uint8_t)s);
    return JS_UNDEFINED;
}

static JSValue jsb_display_setTextAlign(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    /* Core display HAL has no text-alignment state; ignored. */
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawRect(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, w = 0, h = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &w);
    jsb_arg_int(ctx, args_ptr, 3, &h);
    jsb_arg_int(ctx, args_ptr, 4, &c);
    display__draw_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawFillRect(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, w = 0, h = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &w);
    jsb_arg_int(ctx, args_ptr, 3, &h);
    jsb_arg_int(ctx, args_ptr, 4, &c);
    display__fill_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawFillRectGradient(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    /* Gradient rectangles are not supported by the Core display HAL. */
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawRoundRect(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, w = 0, h = 0, r = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &w);
    jsb_arg_int(ctx, args_ptr, 3, &h);
    jsb_arg_int(ctx, args_ptr, 4, &r);
    jsb_arg_int(ctx, args_ptr, 5, &c);
    display__draw_round_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)r, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawFillRoundRect(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, w = 0, h = 0, r = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &w);
    jsb_arg_int(ctx, args_ptr, 3, &h);
    jsb_arg_int(ctx, args_ptr, 4, &r);
    jsb_arg_int(ctx, args_ptr, 5, &c);
    display__fill_round_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)r, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawTriangle(JSContext *ctx, JSValue *args_ptr)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x0);
    jsb_arg_int(ctx, args_ptr, 1, &y0);
    jsb_arg_int(ctx, args_ptr, 2, &x1);
    jsb_arg_int(ctx, args_ptr, 3, &y1);
    jsb_arg_int(ctx, args_ptr, 4, &x2);
    jsb_arg_int(ctx, args_ptr, 5, &y2);
    jsb_arg_int(ctx, args_ptr, 6, &c);
    display__draw_triangle((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2,
                           (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawFillTriangle(JSContext *ctx, JSValue *args_ptr)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x0);
    jsb_arg_int(ctx, args_ptr, 1, &y0);
    jsb_arg_int(ctx, args_ptr, 2, &x1);
    jsb_arg_int(ctx, args_ptr, 3, &y1);
    jsb_arg_int(ctx, args_ptr, 4, &x2);
    jsb_arg_int(ctx, args_ptr, 5, &y2);
    jsb_arg_int(ctx, args_ptr, 6, &c);
    display__fill_triangle((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2,
                           (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawCircle(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, r = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &r);
    jsb_arg_int(ctx, args_ptr, 3, &c);
    display__draw_circle((int16_t)x, (int16_t)y, (int16_t)r, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawFillCircle(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, r = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &r);
    jsb_arg_int(ctx, args_ptr, 3, &c);
    display__fill_circle((int16_t)x, (int16_t)y, (int16_t)r, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawArc(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawWideLine(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawLine(JSContext *ctx, JSValue *args_ptr)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x0);
    jsb_arg_int(ctx, args_ptr, 1, &y0);
    jsb_arg_int(ctx, args_ptr, 2, &x1);
    jsb_arg_int(ctx, args_ptr, 3, &y1);
    jsb_arg_int(ctx, args_ptr, 4, &c);
    display__draw_line((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawFastVLine(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, h = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &h);
    jsb_arg_int(ctx, args_ptr, 3, &c);
    display__draw_line((int16_t)x, (int16_t)y, (int16_t)x, (int16_t)(y + h - 1), (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawFastHLine(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, w = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &w);
    jsb_arg_int(ctx, args_ptr, 3, &c);
    display__draw_line((int16_t)x, (int16_t)y, (int16_t)(x + w - 1), (int16_t)y, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawPixel(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 2, &c);
    display__draw_pixel((int16_t)x, (int16_t)y, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawBitmap(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, w = 0, h = 0, bpp = 8;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 3, &w);
    jsb_arg_int(ctx, args_ptr, 4, &h);
    jsb_arg_int(ctx, args_ptr, 5, &bpp);

    JSValue data = jsb_get_arg(ctx, args_ptr, 2);
    size_t data_len = 0;
    const uint8_t *data_ptr = NULL;
    if (JS_IsString(ctx, data)) {
        JSCStringBuf buf;
        data_ptr = (const uint8_t *)JS_ToCStringLen(ctx, &data_len, data, &buf);
    } else if (JS_IsObject(ctx, data)) {
        data_ptr = (const uint8_t *)JS_GetTypedArrayBuffer(ctx, &data_len, data);
    }

    if (data_ptr == NULL) {
        return JS_ThrowTypeError(ctx, "drawBitmap: expected string/ArrayBuffer/Uint8Array");
    }
    if (bpp == 16) {
        display__draw_rgb_bitmap((int16_t)x, (int16_t)y, (const uint16_t *)data_ptr, (int16_t)w, (int16_t)h);
    } else if (bpp == 1) {
        display__draw_bitmap((int16_t)x, (int16_t)y, data_ptr, (int16_t)w, (int16_t)h, BRUCE_COLOR_WHITE);
    }
    /* 8/4bpp are not supported by the Core display HAL. */
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawXBitmap(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0, w = 0, h = 0, fg = BRUCE_COLOR_WHITE, bg = BRUCE_COLOR_BLACK;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    jsb_arg_int(ctx, args_ptr, 3, &w);
    jsb_arg_int(ctx, args_ptr, 4, &h);
    jsb_arg_int(ctx, args_ptr, 5, &fg);
    jsb_arg_int(ctx, args_ptr, 6, &bg);

    JSValue data = jsb_get_arg(ctx, args_ptr, 2);
    size_t data_len = 0;
    const uint8_t *data_ptr = NULL;
    if (JS_IsString(ctx, data)) {
        JSCStringBuf buf;
        data_ptr = (const uint8_t *)JS_ToCStringLen(ctx, &data_len, data, &buf);
    } else if (JS_IsObject(ctx, data)) {
        data_ptr = (const uint8_t *)JS_GetTypedArrayBuffer(ctx, &data_len, data);
    }

    if (data_ptr == NULL) {
        return JS_ThrowTypeError(ctx, "drawXBitmap: expected string/ArrayBuffer/Uint8Array");
    }
    display__draw_xbitmap((int16_t)x, (int16_t)y, data_ptr, (int16_t)w, (int16_t)h, (bruce_display_color_t)fg);
    (void)data_len;
    (void)bg;
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawString(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *s = jsb_arg_string(ctx, args_ptr, 0, &buf);
    int x = 0, y = 0;
    jsb_arg_int(ctx, args_ptr, 1, &x);
    jsb_arg_int(ctx, args_ptr, 2, &y);
    if (s != NULL) {
        display__set_cursor((int16_t)x, (int16_t)y);
        display__print(s);
    }
    return JS_UNDEFINED;
}

static JSValue jsb_display_setCursor(JSContext *ctx, JSValue *args_ptr)
{
    int x = 0, y = 0;
    jsb_arg_int(ctx, args_ptr, 0, &x);
    jsb_arg_int(ctx, args_ptr, 1, &y);
    display__set_cursor((int16_t)x, (int16_t)y);
    return JS_UNDEFINED;
}

static JSValue jsb_display_print(JSContext *ctx, JSValue *args_ptr)
{
    jsb_print_args(ctx, args_ptr, false);
    return JS_UNDEFINED;
}

static JSValue jsb_display_println(JSContext *ctx, JSValue *args_ptr)
{
    jsb_print_args(ctx, args_ptr, true);
    return JS_UNDEFINED;
}

static JSValue jsb_display_fillScreen(JSContext *ctx, JSValue *args_ptr)
{
    int c = 0;
    jsb_arg_int(ctx, args_ptr, 0, &c);
    display__fill_screen((bruce_display_color_t)c);
    return JS_UNDEFINED;
}

static JSValue jsb_display_width(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewInt32(ctx, display__width());
}

static JSValue jsb_display_height(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewInt32(ctx, display__height());
}

static JSValue jsb_display_drawImage(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawJpg(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    return JS_UNDEFINED;
}

static JSValue jsb_display_drawGif(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    return JS_UNDEFINED;
}

static JSValue jsb_display_getRotation(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewInt32(ctx, display__get_rotation());
}

static JSValue jsb_display_getBrightness(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewInt32(ctx, display__get_brightness());
}

static JSValue jsb_display_setBrightness(JSContext *ctx, JSValue *args_ptr)
{
    int v = 0;
    jsb_arg_int(ctx, args_ptr, 0, &v);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    display__set_brightness((uint8_t)v);
    return JS_NewInt32(ctx, 1);
}

static JSValue jsb_display_restoreBrightness(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    return JS_UNDEFINED;
}

static JSValue jsb_display_flush(JSContext *ctx, JSValue *args_ptr)
{
    (void)args_ptr;
    return JS_NewInt32(ctx, (int)display__flush());
}

static const js_binding_t s_display_bindings[] = {
    {"display.color", jsb_display_color},
    {"display.setTextColor", jsb_display_setTextColor},
    {"display.setTextSize", jsb_display_setTextSize},
    {"display.setTextAlign", jsb_display_setTextAlign},
    {"display.drawRect", jsb_display_drawRect},
    {"display.drawFillRect", jsb_display_drawFillRect},
    {"display.drawFillRectGradient", jsb_display_drawFillRectGradient},
    {"display.drawRoundRect", jsb_display_drawRoundRect},
    {"display.drawFillRoundRect", jsb_display_drawFillRoundRect},
    {"display.drawTriangle", jsb_display_drawTriangle},
    {"display.drawFillTriangle", jsb_display_drawFillTriangle},
    {"display.drawCircle", jsb_display_drawCircle},
    {"display.drawFillCircle", jsb_display_drawFillCircle},
    {"display.drawArc", jsb_display_drawArc},
    {"display.drawWideLine", jsb_display_drawWideLine},
    {"display.drawLine", jsb_display_drawLine},
    {"display.drawFastVLine", jsb_display_drawFastVLine},
    {"display.drawFastHLine", jsb_display_drawFastHLine},
    {"display.drawPixel", jsb_display_drawPixel},
    {"display.drawBitmap", jsb_display_drawBitmap},
    {"display.drawXBitmap", jsb_display_drawXBitmap},
    {"display.drawString", jsb_display_drawString},
    {"display.setCursor", jsb_display_setCursor},
    {"display.print", jsb_display_print},
    {"display.println", jsb_display_println},
    {"display.fillScreen", jsb_display_fillScreen},
    {"display.width", jsb_display_width},
    {"display.height", jsb_display_height},
    {"display.drawImage", jsb_display_drawImage},
    {"display.drawJpg", jsb_display_drawJpg},
    {"display.drawGif", jsb_display_drawGif},
    {"display.getRotation", jsb_display_getRotation},
    {"display.getBrightness", jsb_display_getBrightness},
    {"display.setBrightness", jsb_display_setBrightness},
    {"display.restoreBrightness", jsb_display_restoreBrightness},
    {"display.flush", jsb_display_flush},
};

static const char s_display_setup[] =
    "(function(){"
    "var bridge=globalThis.__bruce_bridge;"
    "if(typeof bridge!=='function')return;"
    "globalThis.display=globalThis.display||{};"
    "var methods=['color','setTextColor','setTextSize','setTextAlign','drawRect','drawFillRect','drawFillRectGradient','drawRoundRect','drawFillRoundRect','drawTriangle','drawFillTriangle','drawCircle','drawFillCircle','drawArc','drawWideLine','drawLine','drawFastVLine','drawFastHLine','drawPixel','drawBitmap','drawXBitmap','drawString','setCursor','print','println','fillScreen','width','height','drawImage','drawJpg','drawGif','getRotation','getBrightness','setBrightness','restoreBrightness','flush'];"
    "for(var i=0;i<methods.length;i++){"
    "(function(full,method){"
    "globalThis.display[method]=function(){return bridge(full,Array.prototype.slice.call(arguments));};"
    "})('display.'+methods[i],methods[i]);"
    "}"
    "})();";

void jsb_display__init(JSContext *ctx)
{
    js_bindings__add_module(s_display_bindings,
                            sizeof(s_display_bindings) / sizeof(s_display_bindings[0]));
    JSValue result = JS_Eval(ctx, s_display_setup, sizeof(s_display_setup) - 1, "js_display_setup", 0);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        printf("[js_display] setup error: ");
        JS_PrintValueF(ctx, ex, JS_DUMP_LONG);
        printf("\n");
    }
}
