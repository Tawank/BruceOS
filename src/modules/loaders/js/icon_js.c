#include "icon_js.h"

#include "core_sdk/display.h"
#include "core_sdk/icon.h"

#include "native_helpers_js.h"

JSValue native_iconDraw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    JSCStringBuf buf;
    const char *name = js_native_arg_string(ctx, argc, argv, 0, &buf);
    if (name == NULL) { return JS_UNDEFINED; }
    const bruce_icon_t *icon = icon__get(name);
    if (icon == NULL) { return JS_UNDEFINED; }
    int x = js_native_arg_int(ctx, argc, argv, 1, 0);
    int y = js_native_arg_int(ctx, argc, argv, 2, 0);
    int size = js_native_arg_int(ctx, argc, argv, 3, BRUCE_ICON_SIZE);
    int c = js_native_arg_int(ctx, argc, argv, 4, 0xFFFF);
    display__draw_bitmap_scaled(
        (int16_t)x, (int16_t)y, icon->bits, icon->width, icon->height, (int16_t)size, (int16_t)size,
        (bruce_display_color_t)c
    );
    return JS_UNDEFINED;
}
