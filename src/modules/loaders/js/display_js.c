#include "display_js.h"

#include "core_sdk/display.h"
#include "core_sdk/image.h"

#include "native_helpers_js.h"

/* -------------------------------------------------------------------------- */
/* Legacy mquickjs stdlib native functions (direct C bindings)             */
/* -------------------------------------------------------------------------- */

static int native_arg_int(JSContext *ctx, int argc, JSValue *argv, int idx, int def) {
    if (idx >= argc || !JS_IsNumber(ctx, argv[idx])) { return def; }
    int v = def;
    JS_ToInt32(ctx, &v, argv[idx]);
    return v;
}

static const char *native_arg_string(JSContext *ctx, int argc, JSValue *argv, int idx, JSCStringBuf *buf) {
    if (idx >= argc || !JS_IsString(ctx, argv[idx])) { return NULL; }
    return JS_ToCString(ctx, argv[idx], buf);
}

JSValue native_beginFrame(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__begin_frame());
}

JSValue native_present(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__present());
}

JSValue native_flush(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    bruce_result_t result = display__present();
    if (result == BRUCE_ERR_INVALID_STATE) {
        result = display__begin_frame();
        if (result == BRUCE_OK) result = display__present();
    }
    return JS_NewInt32(ctx, result);
}

JSValue native_color(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int r = native_arg_int(ctx, argc, argv, 0, 0);
    int g = native_arg_int(ctx, argc, argv, 1, 0);
    int b = native_arg_int(ctx, argc, argv, 2, 0);
    int mode = native_arg_int(ctx, argc, argv, 3, 16);
    int color = (int)display__color565((uint8_t)r, (uint8_t)g, (uint8_t)b);
    if (mode == 16) {
        return JS_NewInt32(ctx, color);
    } else {
        return JS_NewInt32(ctx, ((color & 0xE000) >> 8) | ((color & 0x0700) >> 6) | ((color & 0x0018) >> 3));
    }
}

JSValue native_setTextColor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int c = native_arg_int(ctx, argc, argv, 0, 0);
    int bg = native_arg_int(ctx, argc, argv, 1, -1);
    display__set_text_color((bruce_display_color_t)c);
    if (bg >= 0) { display__set_text_bg_color((uint32_t)bg); }
    return JS_UNDEFINED;
}

JSValue native_setTextSize(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int s = native_arg_int(ctx, argc, argv, 0, 1);
    display__set_text_size((uint8_t)s);
    return JS_UNDEFINED;
}

JSValue native_setTextAlign(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    /* Core display HAL has no text-alignment state; ignored. */
    return JS_UNDEFINED;
}

JSValue native_drawString(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    JSCStringBuf buf;
    const char *s = native_arg_string(ctx, argc, argv, 0, &buf);
    int x = native_arg_int(ctx, argc, argv, 1, 0);
    int y = native_arg_int(ctx, argc, argv, 2, 0);
    if (s != NULL) {
        display__set_cursor((int16_t)x, (int16_t)y);
        display__print(s);
    }
    return JS_UNDEFINED;
}

JSValue native_setCursor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    display__set_cursor((int16_t)x, (int16_t)y);
    return JS_UNDEFINED;
}

JSValue native_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    for (int i = 0; i < argc && i < 20; i++) {
        if (i > 0) { display__print(" "); }
        if (JS_IsString(ctx, argv[i])) {
            JSCStringBuf buf;
            const char *s = JS_ToCString(ctx, argv[i], &buf);
            if (s != NULL) { display__print(s); }
        } else {
            JSValue str = JS_ToString(ctx, argv[i]);
            JSCStringBuf buf;
            const char *s = JS_ToCString(ctx, str, &buf);
            if (s != NULL) { display__print(s); }
        }
    }
    return JS_UNDEFINED;
}

JSValue native_println(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    native_print(ctx, this_val, argc, argv);
    display__print("\n");
    return JS_UNDEFINED;
}

JSValue native_fillScreen(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int c = native_arg_int(ctx, argc, argv, 0, 0);
    display__fill_screen((bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_width(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__width());
}

JSValue native_height(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__height());
}

JSValue native_drawPixel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int c = native_arg_int(ctx, argc, argv, 2, 0);
    display__draw_pixel((int16_t)x, (int16_t)y, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x0 = native_arg_int(ctx, argc, argv, 0, 0);
    int y0 = native_arg_int(ctx, argc, argv, 1, 0);
    int x1 = native_arg_int(ctx, argc, argv, 2, 0);
    int y1 = native_arg_int(ctx, argc, argv, 3, 0);
    int c = native_arg_int(ctx, argc, argv, 4, 0);
    display__draw_line((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawWideLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    /* Core display HAL does not support wide lines. */
    return JS_UNDEFINED;
}

JSValue native_drawFastVLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int h = native_arg_int(ctx, argc, argv, 2, 0);
    int c = native_arg_int(ctx, argc, argv, 3, 0);
    display__draw_line((int16_t)x, (int16_t)y, (int16_t)x, (int16_t)(y + h - 1), (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawFastHLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 2, 0);
    int c = native_arg_int(ctx, argc, argv, 3, 0);
    display__draw_line((int16_t)x, (int16_t)y, (int16_t)(x + w - 1), (int16_t)y, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 2, 0);
    int h = native_arg_int(ctx, argc, argv, 3, 0);
    int c = native_arg_int(ctx, argc, argv, 4, 0);
    display__draw_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawFillRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 2, 0);
    int h = native_arg_int(ctx, argc, argv, 3, 0);
    int c = native_arg_int(ctx, argc, argv, 4, 0);
    display__fill_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawFillRectGradient(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    /* Gradient rectangles are not supported by the Core display HAL. */
    return JS_UNDEFINED;
}

JSValue native_drawRoundRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 2, 0);
    int h = native_arg_int(ctx, argc, argv, 3, 0);
    int r = native_arg_int(ctx, argc, argv, 4, 0);
    int c = native_arg_int(ctx, argc, argv, 5, 0);
    display__draw_round_rect(
        (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)r, (bruce_display_color_t)c
    );
    return JS_UNDEFINED;
}

JSValue native_drawFillRoundRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 2, 0);
    int h = native_arg_int(ctx, argc, argv, 3, 0);
    int r = native_arg_int(ctx, argc, argv, 4, 0);
    int c = native_arg_int(ctx, argc, argv, 5, 0);
    display__fill_round_rect(
        (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)r, (bruce_display_color_t)c
    );
    return JS_UNDEFINED;
}

JSValue native_drawTriangle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x0 = native_arg_int(ctx, argc, argv, 0, 0);
    int y0 = native_arg_int(ctx, argc, argv, 1, 0);
    int x1 = native_arg_int(ctx, argc, argv, 2, 0);
    int y1 = native_arg_int(ctx, argc, argv, 3, 0);
    int x2 = native_arg_int(ctx, argc, argv, 4, 0);
    int y2 = native_arg_int(ctx, argc, argv, 5, 0);
    int c = native_arg_int(ctx, argc, argv, 6, 0);
    display__draw_triangle(
        (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, (bruce_display_color_t)c
    );
    return JS_UNDEFINED;
}

JSValue native_drawFillTriangle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x0 = native_arg_int(ctx, argc, argv, 0, 0);
    int y0 = native_arg_int(ctx, argc, argv, 1, 0);
    int x1 = native_arg_int(ctx, argc, argv, 2, 0);
    int y1 = native_arg_int(ctx, argc, argv, 3, 0);
    int x2 = native_arg_int(ctx, argc, argv, 4, 0);
    int y2 = native_arg_int(ctx, argc, argv, 5, 0);
    int c = native_arg_int(ctx, argc, argv, 6, 0);
    display__fill_triangle(
        (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, (bruce_display_color_t)c
    );
    return JS_UNDEFINED;
}

JSValue native_drawCircle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int r = native_arg_int(ctx, argc, argv, 2, 0);
    int c = native_arg_int(ctx, argc, argv, 3, 0);
    display__draw_circle((int16_t)x, (int16_t)y, (int16_t)r, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawFillCircle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int r = native_arg_int(ctx, argc, argv, 2, 0);
    int c = native_arg_int(ctx, argc, argv, 3, 0);
    display__fill_circle((int16_t)x, (int16_t)y, (int16_t)r, (bruce_display_color_t)c);
    return JS_UNDEFINED;
}

JSValue native_drawBitmap(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 3, 0);
    int h = native_arg_int(ctx, argc, argv, 4, 0);
    int bpp = native_arg_int(ctx, argc, argv, 5, 8);
    int c = native_arg_int(ctx, argc, argv, 6, 0xFFFF);

    JSCStringBuf buf;
    size_t len = 0;
    const uint8_t *data = NULL;
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        data = (const uint8_t *)JS_ToCStringLen(ctx, &len, argv[2], &buf);
    } else if (argc > 2 && JS_IsObject(ctx, argv[2])) {
        data = (const uint8_t *)JS_GetTypedArrayBuffer(ctx, &len, argv[2]);
    }

    if (data != NULL) {
        if (bpp == 16) {
            display__draw_rgb_bitmap((int16_t)x, (int16_t)y, (const uint16_t *)data, (int16_t)w, (int16_t)h);
        } else if (bpp == 1) {
            display__draw_bitmap(
                (int16_t)x, (int16_t)y, data, (int16_t)w, (int16_t)h, (bruce_display_color_t)c
            );
        }
        /* 8/4bpp are not supported by the Core display HAL. */
    }
    return JS_UNDEFINED;
}

JSValue native_drawXBitmap(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 3, 0);
    int h = native_arg_int(ctx, argc, argv, 4, 0);
    int c = native_arg_int(ctx, argc, argv, 5, 0xFFFF);

    JSCStringBuf buf;
    size_t len = 0;
    const uint8_t *data = NULL;
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        data = (const uint8_t *)JS_ToCStringLen(ctx, &len, argv[2], &buf);
    } else if (argc > 2 && JS_IsObject(ctx, argv[2])) {
        data = (const uint8_t *)JS_GetTypedArrayBuffer(ctx, &len, argv[2]);
    }

    if (data != NULL) {
        display__draw_xbitmap((int16_t)x, (int16_t)y, data, (int16_t)w, (int16_t)h, (bruce_display_color_t)c);
    }
    return JS_UNDEFINED;
}

JSValue native_drawArc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int r = native_arg_int(ctx, argc, argv, 2, 0);
    int start = native_arg_int(ctx, argc, argv, 3, 0);
    int end = native_arg_int(ctx, argc, argv, 4, 360);
    int c = native_arg_int(ctx, argc, argv, 5, 0xFFFF);
    display__draw_arc(
        (int16_t)x, (int16_t)y, (int16_t)r, (int16_t)start, (int16_t)end, (bruce_display_color_t)c
    );
    return JS_UNDEFINED;
}

static JSValue native_drawImageCommon(JSContext *ctx, int argc, JSValue *argv) {
    if (argc < 1 || (!JS_IsString(ctx, argv[0]) && !JS_IsTypedArray(ctx, argv[0]))) {
        return JS_ThrowTypeError(ctx, "display image source must be a path or Uint8Array");
    }
    bruce_image_draw_options_t options = {
        .x = (int16_t)native_arg_int(ctx, argc, argv, 1, 0),
        .y = (int16_t)native_arg_int(ctx, argc, argv, 2, 0),
        .center = js_native_arg_bool(ctx, argc, argv, 3, false),
        .fit = js_native_arg_bool(ctx, argc, argv, 4, false),
        .background = BRUCE_COLOR_BLACK,
    };
    bruce_result_t result;
    image_bitmap_t bitmap = {0};
    if (JS_IsString(ctx, argv[0])) {
        JSCStringBuf path_buffer;
        const char *path = JS_ToCString(ctx, argv[0], &path_buffer);
        if (path == NULL) return JS_ThrowTypeError(ctx, "invalid image path");
        result = image__get_bitmap_from_file(path, &options, &bitmap);
    } else {
        size_t size = 0;
        const uint8_t *data = (const uint8_t *)JS_GetTypedArrayBuffer(ctx, &size, argv[0]);
        if (data == NULL || size == 0) return JS_ThrowTypeError(ctx, "empty image data");
        result = image__get_bitmap_from_memory(data, size, &options, &bitmap);
    }
    if (result == BRUCE_OK) result = image__draw_bitmap(&bitmap, &options);
    image__bitmap_release(&bitmap);
    if (result != BRUCE_OK) return JS_ThrowInternalError(ctx, "image decode failed: %d", (int)result);
    return JS_NewBool(true);
}

JSValue native_drawImage(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    return native_drawImageCommon(ctx, argc, argv);
}

JSValue native_drawJpg(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    return native_drawImageCommon(ctx, argc, argv);
}

JSValue native_drawPng(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    return native_drawImageCommon(ctx, argc, argv);
}

JSValue native_drawGif(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    return native_drawImageCommon(ctx, argc, argv);
}

JSValue native_gifOpen(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

JSValue native_getRotation(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__get_rotation());
}

JSValue native_getBrightness(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__get_brightness());
}

JSValue native_setBrightness(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int v = native_arg_int(ctx, argc, argv, 0, 0);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    display__set_brightness((uint8_t)v);
    return JS_NewInt32(ctx, 1);
}

JSValue native_restoreBrightness(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

JSValue native_createSprite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    /* Sprites are not implemented in the Core display HAL. */
    return JS_UNDEFINED;
}

JSValue native_pushSprite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

JSValue native_deleteSprite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

/* -------------------------------------------------------------------------- */
/* Overlays (see core_sdk/display.h)                                          */
/* -------------------------------------------------------------------------- */

JSValue native_screenWidth(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__screen_width());
}

JSValue native_screenHeight(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, display__screen_height());
}

/* Overlay ids returned by display__overlay_create() are always > 0 (see
 * BRUCE_DISPLAY_OVERLAY_ID_INVALID), so the JS surface returns the id
 * itself (a positive int32) on success and a negative bruce_result_t on
 * failure, the same "non-negative is the value, negative is BRUCE_ERR_*"
 * convention bruce_result_t itself uses. */
JSValue native_overlayCreate(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int x = native_arg_int(ctx, argc, argv, 0, 0);
    int y = native_arg_int(ctx, argc, argv, 1, 0);
    int w = native_arg_int(ctx, argc, argv, 2, 0);
    int h = native_arg_int(ctx, argc, argv, 3, 0);
    bruce_display_overlay_id_t overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
    bruce_result_t result =
        display__overlay_create((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, &overlay);
    return JS_NewInt32(ctx, result == BRUCE_OK ? (int32_t)overlay : (int32_t)result);
}

JSValue native_overlayDestroy(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int id = native_arg_int(ctx, argc, argv, 0, 0);
    return JS_NewInt32(ctx, display__overlay_destroy((bruce_display_overlay_id_t)id));
}

JSValue native_overlayShow(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int id = native_arg_int(ctx, argc, argv, 0, 0);
    return JS_NewInt32(ctx, display__overlay_show((bruce_display_overlay_id_t)id));
}

JSValue native_overlayHide(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int id = native_arg_int(ctx, argc, argv, 0, 0);
    return JS_NewInt32(ctx, display__overlay_hide((bruce_display_overlay_id_t)id));
}

JSValue native_overlayMove(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int id = native_arg_int(ctx, argc, argv, 0, 0);
    int x = native_arg_int(ctx, argc, argv, 1, 0);
    int y = native_arg_int(ctx, argc, argv, 2, 0);
    return JS_NewInt32(ctx, display__overlay_move((bruce_display_overlay_id_t)id, (int16_t)x, (int16_t)y));
}

JSValue native_overlayBegin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int id = native_arg_int(ctx, argc, argv, 0, 0);
    return JS_NewInt32(ctx, display__overlay_begin((bruce_display_overlay_id_t)id));
}

JSValue native_overlayEnd(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    int id = native_arg_int(ctx, argc, argv, 0, 0);
    return JS_NewInt32(ctx, display__overlay_end((bruce_display_overlay_id_t)id));
}
