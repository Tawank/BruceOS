#pragma once

#include "mquickjs.h"

JSValue native_color(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_setTextColor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_setTextSize(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_setTextAlign(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawString(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_setCursor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_println(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_fillScreen(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_width(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_height(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawPixel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawWideLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawFastVLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawFastHLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawFillRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawFillRectGradient(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawRoundRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawFillRoundRect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawTriangle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawFillTriangle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawCircle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawFillCircle(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawBitmap(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawXBitmap(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawArc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawJpg(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_drawGif(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_gifOpen(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getRotation(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_getBrightness(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_setBrightness(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_restoreBrightness(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_createSprite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_pushSprite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_deleteSprite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
