#include "keyboard_js.h"

#include "core_sdk/dialog.h"
#include "core_sdk/input.h"

#include <stdlib.h>

#define KEYBOARD_INPUT_BUFFER_SIZE 256

static int keyboard_arg_int(JSContext *ctx, int argc, JSValue *argv, int idx, int def) {
    if (idx >= argc || !JS_IsNumber(ctx, argv[idx])) { return def; }
    int v = def;
    JS_ToInt32(ctx, &v, argv[idx]);
    return v;
}

static const char *keyboard_arg_string(JSContext *ctx, int argc, JSValue *argv, int idx, JSCStringBuf *buf) {
    if (idx >= argc || !JS_IsString(ctx, argv[idx])) { return NULL; }
    return JS_ToCString(ctx, argv[idx], buf);
}

static JSValue keyboard_input_result(JSContext *ctx, char *buffer) {
    if (buffer[0] == '\x1B') { buffer[0] = '\0'; }
    return JS_NewString(ctx, buffer);
}

JSValue native_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    char buffer[KEYBOARD_INPUT_BUFFER_SIZE];
    buffer[0] = '\0';

    const char *title = "";
    const char *prompt = "";
    const char *initial = "";
    bool mask = false;
    JSCStringBuf title_buf;
    JSCStringBuf prompt_buf;
    JSCStringBuf initial_buf;

    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        title = keyboard_arg_string(ctx, argc, argv, 0, &title_buf);
    } else if (argc > 0 && JS_IsNumber(ctx, argv[0])) {
        int max_size = keyboard_arg_int(ctx, argc, argv, 0, 0);
        (void)max_size;
        if (argc > 1 && JS_IsString(ctx, argv[1])) {
            prompt = keyboard_arg_string(ctx, argc, argv, 1, &prompt_buf);
        }
        if (argc > 2 && JS_IsBool(argv[2])) { mask = JS_ToBool(ctx, argv[2]) != 0; }
    }

    if (argc > 1 && JS_IsString(ctx, argv[1])) {
        prompt = keyboard_arg_string(ctx, argc, argv, 1, &prompt_buf);
    }
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        initial = keyboard_arg_string(ctx, argc, argv, 2, &initial_buf);
    }
    if (argc > 3 && JS_IsBool(argv[argc - 1])) { mask = JS_ToBool(ctx, argv[argc - 1]) != 0; }

    bruce_result_t result = dialog__text_input(title, prompt, initial, mask, buffer, sizeof(buffer));
    if (result != BRUCE_OK) { buffer[0] = '\0'; }
    return keyboard_input_result(ctx, buffer);
}

JSValue native_num_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    char buffer[KEYBOARD_INPUT_BUFFER_SIZE];
    buffer[0] = '\0';

    const char *title = "";
    const char *prompt = "";
    const char *initial = "";
    JSCStringBuf title_buf;
    JSCStringBuf prompt_buf;
    JSCStringBuf initial_buf;

    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        title = keyboard_arg_string(ctx, argc, argv, 0, &title_buf);
    }
    if (argc > 1 && JS_IsString(ctx, argv[1])) {
        prompt = keyboard_arg_string(ctx, argc, argv, 1, &prompt_buf);
    }
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        initial = keyboard_arg_string(ctx, argc, argv, 2, &initial_buf);
    }

    bruce_result_t result = dialog__number_input(title, prompt, initial, buffer, sizeof(buffer));
    if (result != BRUCE_OK) { buffer[0] = '\0'; }
    return keyboard_input_result(ctx, buffer);
}

JSValue native_hex_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    char buffer[KEYBOARD_INPUT_BUFFER_SIZE];
    buffer[0] = '\0';

    const char *title = "";
    const char *prompt = "";
    const char *initial = "";
    JSCStringBuf title_buf;
    JSCStringBuf prompt_buf;
    JSCStringBuf initial_buf;

    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        title = keyboard_arg_string(ctx, argc, argv, 0, &title_buf);
    }
    if (argc > 1 && JS_IsString(ctx, argv[1])) {
        prompt = keyboard_arg_string(ctx, argc, argv, 1, &prompt_buf);
    }
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        initial = keyboard_arg_string(ctx, argc, argv, 2, &initial_buf);
    }

    bruce_result_t result = dialog__hex_input(title, prompt, initial, buffer, sizeof(buffer));
    if (result != BRUCE_OK) { buffer[0] = '\0'; }
    return keyboard_input_result(ctx, buffer);
}

JSValue native_getKeysPressed(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    /* The Core input HAL exposes events, not a key matrix. Return an empty array. */
    return JS_NewArray(ctx, 0);
}

JSValue native_getPrevPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(input__check(BRUCE_INPUT_CODE_UP, true));
}

JSValue native_getSelPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(input__check(BRUCE_INPUT_CODE_SELECT, true));
}

JSValue native_getEscPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(input__check(BRUCE_INPUT_CODE_BACK, true));
}

JSValue native_getNextPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(input__check(BRUCE_INPUT_CODE_DOWN, true));
}

JSValue native_getAnyPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    if (input__check(BRUCE_INPUT_CODE_UP, true)) return JS_NewBool(1);
    if (input__check(BRUCE_INPUT_CODE_DOWN, true)) return JS_NewBool(1);
    if (input__check(BRUCE_INPUT_CODE_LEFT, true)) return JS_NewBool(1);
    if (input__check(BRUCE_INPUT_CODE_RIGHT, true)) return JS_NewBool(1);
    if (input__check(BRUCE_INPUT_CODE_SELECT, true)) return JS_NewBool(1);
    if (input__check(BRUCE_INPUT_CODE_BACK, true)) return JS_NewBool(1);
    if (input__check(BRUCE_INPUT_CODE_MENU, true)) return JS_NewBool(1);
    if (input__check(BRUCE_INPUT_CODE_HOME, true)) return JS_NewBool(1);
    return JS_NewBool(0);
}

JSValue native_setLongPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    /* Long-press handling is not exposed by the Core input HAL. */
    return JS_UNDEFINED;
}

/* Drains one event from the Core input event loop (see core_sdk/input.h).
 * Unlike the getXPress() helpers, which only remove a press event matching
 * one specific code, this surfaces every queued event (press, release, and
 * any code) so a caller can fully drain the queue each frame. Returns null
 * when no event is queued. */
JSValue native_pollEvent(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;

    bruce_input_event_t event;
    if (input__poll(&event) != BRUCE_OK) { return JS_NULL; }

    const char *action = "change";
    if (event.action == BRUCE_INPUT_PRESS) {
        action = "press";
    } else if (event.action == BRUCE_INPUT_RELEASE) {
        action = "release";
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "code", JS_NewInt32(ctx, event.code));
    JS_SetPropertyStr(ctx, obj, "action", JS_NewString(ctx, action));
    JS_SetPropertyStr(ctx, obj, "value", JS_NewInt32(ctx, event.value));
    return obj;
}
