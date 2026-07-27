#include "dialog_js.h"
#include "native_helpers_js.h"
#include "user_classes_js.h"

#include "core_sdk/dialog.h"
#include "core_sdk/storage.h"

JSValue native_dialogMessage(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "dialog.message(msg:string, buttons?:object)");
    }

    JSCStringBuf msg_buf;
    const char *msg = JS_ToCString(ctx, argv[0], &msg_buf);

    const char *left = NULL;
    const char *center = NULL;
    const char *right = NULL;
    JSCStringBuf left_buf, center_buf, right_buf;

    if (argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "left");
        if (JS_IsString(ctx, v)) { left = JS_ToCString(ctx, v, &left_buf); }
        v = JS_GetPropertyStr(ctx, argv[1], "center");
        if (JS_IsString(ctx, v)) { center = JS_ToCString(ctx, v, &center_buf); }
        v = JS_GetPropertyStr(ctx, argv[1], "right");
        if (JS_IsString(ctx, v)) { right = JS_ToCString(ctx, v, &right_buf); }
    }

    bruce_dialog_choice_t choices[3];
    size_t choice_count = 0;
    if (left != NULL) {
        choices[choice_count].label = left;
        choices[choice_count].value = left;
        choice_count++;
    }
    if (center != NULL) {
        choices[choice_count].label = center;
        choices[choice_count].value = center;
        choice_count++;
    }
    if (right != NULL) {
        choices[choice_count].label = right;
        choices[choice_count].value = right;
        choice_count++;
    }

    if (choice_count == 0) {
        dialog__message(BRUCE_DIALOG_INFO, NULL, msg);
        return JS_NewString(ctx, "right");
    }

    size_t selected = 0;
    bruce_result_t result = dialog__choice(NULL, msg, choices, choice_count, &selected, NULL);
    if (result != BRUCE_OK) { return JS_NewString(ctx, ""); }

    const char *rv = "right";
    if (selected == 0 && left != NULL) {
        rv = left;
    } else if (selected == 1 && center != NULL) {
        rv = center;
    } else if (right != NULL) {
        rv = right;
    }
    return JS_NewString(ctx, rv);
}

JSValue native_dialogInfo(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) { return JS_UNDEFINED; }
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    if (s != NULL) { dialog__message(BRUCE_DIALOG_INFO, NULL, s); }
    return JS_UNDEFINED;
}

JSValue native_dialogSuccess(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) { return JS_UNDEFINED; }
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    if (s != NULL) { dialog__message(BRUCE_DIALOG_SUCCESS, NULL, s); }
    return JS_UNDEFINED;
}

JSValue native_dialogWarning(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) { return JS_UNDEFINED; }
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    if (s != NULL) { dialog__message(BRUCE_DIALOG_WARNING, NULL, s); }
    return JS_UNDEFINED;
}

JSValue native_dialogError(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) { return JS_UNDEFINED; }
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, argv[0], &sb);
    if (s != NULL) { dialog__message(BRUCE_DIALOG_ERROR, NULL, s); }
    return JS_UNDEFINED;
}

JSValue native_dialogPickFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    JSCStringBuf path_buf, ext_buf;
    const char *path = js_native_arg_string(ctx, argc, argv, 0, &path_buf);
    const char *ext = js_native_arg_string(ctx, argc, argv, 1, &ext_buf);
    char out[BRUCE_STORAGE_PATH_MAX];
    out[0] = '\0';
    bruce_result_t result =
        dialog__pick_file(path != NULL ? path : "/", ext != NULL ? ext : "*", out, sizeof(out));
    if (result != BRUCE_OK) { return JS_NewString(ctx, ""); }
    return JS_NewString(ctx, out);
}

JSValue native_dialogChoice(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsObject(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "dialog.choice(choices: object|array)");
    }

    JSValue choices_in = argv[0];
    bruce_dialog_choice_t choices[32];
    size_t choice_count = 0;
    char value_storage[32][64];

    if (JS_GetClassID(ctx, choices_in) == JS_CLASS_ARRAY) {
        uint32_t len = js_native_array_length(ctx, choices_in);
        for (uint32_t i = 0; i < len && choice_count < 32; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, choices_in, i);
            if (JS_IsString(ctx, item)) {
                JSCStringBuf sb;
                const char *s = JS_ToCString(ctx, item, &sb);
                if (s != NULL) {
                    choices[choice_count].label = s;
                    choices[choice_count].value = s;
                    choice_count++;
                }
            } else if (JS_IsObject(ctx, item) && JS_GetClassID(ctx, item) == JS_CLASS_ARRAY) {
                JSValue key = JS_GetPropertyUint32(ctx, item, 0);
                JSValue val = JS_GetPropertyUint32(ctx, item, 1);
                if (JS_IsString(ctx, key) && JS_IsString(ctx, val) && choice_count < 32) {
                    JSCStringBuf kb, vb;
                    const char *k = JS_ToCString(ctx, key, &kb);
                    const char *v = JS_ToCString(ctx, val, &vb);
                    if (k != NULL && v != NULL) {
                        strncpy(value_storage[choice_count], v, sizeof(value_storage[0]) - 1);
                        value_storage[choice_count][sizeof(value_storage[0]) - 1] = '\0';
                        choices[choice_count].label = k;
                        choices[choice_count].value = value_storage[choice_count];
                        choice_count++;
                    }
                }
            }
        }
    } else {
        uint32_t prop_count = 0;
        for (uint32_t idx = 0; choice_count < 32; ++idx) {
            const char *key = JS_GetOwnPropertyByIndex(ctx, idx, &prop_count, choices_in);
            if (key == NULL) { break; }
            JSValue val = JS_GetPropertyStr(ctx, choices_in, key);
            if (JS_IsString(ctx, val)) {
                JSCStringBuf vb;
                const char *v = JS_ToCString(ctx, val, &vb);
                if (v != NULL) {
                    strncpy(value_storage[choice_count], v, sizeof(value_storage[0]) - 1);
                    value_storage[choice_count][sizeof(value_storage[0]) - 1] = '\0';
                    choices[choice_count].label = key;
                    choices[choice_count].value = value_storage[choice_count];
                    choice_count++;
                }
            }
        }
    }

    size_t selected = 0;
    bruce_result_t result = dialog__choice(NULL, NULL, choices, choice_count, &selected, NULL);
    if (result != BRUCE_OK || selected >= choice_count) { return JS_NewString(ctx, ""); }
    return JS_NewString(ctx, choices[selected].value);
}

JSValue native_dialogViewFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    /* dialog.viewFile() is intentionally not migrated; use createTextViewer(). */
    return JS_UNDEFINED;
}

JSValue native_dialogViewText(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "dialog.viewText(text:string, title?:string)");
    }
    JSCStringBuf sb;
    const char *text = JS_ToCString(ctx, argv[0], &sb);
    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    dialog__create_text_viewer(NULL, text != NULL ? text : "", &viewer);
    return JS_NewUint32(ctx, viewer);
}

/* -------------------------------------------------------------------------- */
/* TextViewer object                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    bruce_viewer_id_t viewer;
} js_textviewer_data_t;

void native_textviewer_finalizer(JSContext *ctx, void *opaque) {
    (void)ctx;
    js_textviewer_data_t *d = (js_textviewer_data_t *)opaque;
    if (d == NULL) { return; }
    if (d->viewer != BRUCE_VIEWER_ID_INVALID) { dialog__viewer_close(d->viewer); }
    free(d);
}

static bruce_viewer_id_t js_textviewer_get_id(JSContext *ctx, JSValue obj) {
    if (!JS_IsObject(ctx, obj) || JS_GetClassID(ctx, obj) != JS_CLASS_TEXTVIEWER) {
        return BRUCE_VIEWER_ID_INVALID;
    }
    js_textviewer_data_t *d = (js_textviewer_data_t *)JS_GetOpaque(ctx, obj);
    if (d == NULL) { return BRUCE_VIEWER_ID_INVALID; }
    return d->viewer;
}

JSValue native_dialogCreateTextViewer(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) { return JS_ThrowTypeError(ctx, "TextViewer requires text"); }

    JSCStringBuf sb;
    const char *text = JS_ToCString(ctx, argv[0], &sb);
    const char *title = NULL;
    JSCStringBuf title_buf;
    if (argc > 1 && JS_IsString(ctx, argv[1])) { title = JS_ToCString(ctx, argv[1], &title_buf); }

    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    bruce_result_t result = dialog__create_text_viewer(title, text != NULL ? text : "", &viewer);
    if (result != BRUCE_OK) { return JS_ThrowInternalError(ctx, "createTextViewer failed"); }

    JSValue obj = JS_NewObjectClassUser(ctx, JS_CLASS_TEXTVIEWER);
    if (JS_IsException(obj)) {
        dialog__viewer_close(viewer);
        return obj;
    }

    js_textviewer_data_t *d = (js_textviewer_data_t *)malloc(sizeof(js_textviewer_data_t));
    if (d == NULL) {
        dialog__viewer_close(viewer);
        return JS_ThrowOutOfMemory(ctx);
    }
    d->viewer = viewer;
    JS_SetOpaque(ctx, obj, d);

    return obj;
}

JSValue native_dialogCreateTextViewerDraw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)argc;
    (void)argv;
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    (void)viewer;
    /* Core text viewer is drawn by the dialog system; no explicit draw API. */
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerScrollUp(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)argc;
    (void)argv;
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    dialog__viewer_scroll(viewer, -1);
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerScrollDown(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)argc;
    (void)argv;
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    dialog__viewer_scroll(viewer, 1);
    return JS_UNDEFINED;
}

JSValue
native_dialogCreateTextViewerScrollToLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    (void)viewer;
    (void)argc;
    (void)argv;
    /* Absolute line scrolling is not exposed by the Core dialog API. */
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerGetLine(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    (void)viewer;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, "");
}

JSValue native_dialogCreateTextViewerGetMaxLines(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    (void)viewer;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, 0);
}

JSValue
native_dialogCreateTextViewerGetVisibleText(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    (void)viewer;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, "");
}

JSValue native_dialogCreateTextViewerClear(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    dialog__viewer_set_text(viewer, "");
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerFromString(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    bruce_viewer_id_t viewer = js_textviewer_get_id(ctx, *this_val);
    if (viewer == BRUCE_VIEWER_ID_INVALID) { return JS_ThrowTypeError(ctx, "TextViewer: does not exist"); }
    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        JSCStringBuf sb;
        const char *s = JS_ToCString(ctx, argv[0], &sb);
        dialog__viewer_set_text(viewer, s != NULL ? s : "");
    }
    return JS_UNDEFINED;
}

JSValue native_dialogCreateTextViewerClose(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    JSValue obj = JS_UNDEFINED;
    if (argc > 0 && JS_IsObject(ctx, argv[0])) {
        obj = argv[0];
    } else if (JS_IsObject(ctx, *this_val)) {
        obj = *this_val;
    }

    if (!JS_IsObject(ctx, obj) || JS_GetClassID(ctx, obj) != JS_CLASS_TEXTVIEWER) { return JS_UNDEFINED; }

    js_textviewer_data_t *d = (js_textviewer_data_t *)JS_GetOpaque(ctx, obj);
    if (d != NULL && d->viewer != BRUCE_VIEWER_ID_INVALID) {
        dialog__viewer_close(d->viewer);
        d->viewer = BRUCE_VIEWER_ID_INVALID;
    }
    JS_SetOpaque(ctx, obj, NULL);
    return JS_UNDEFINED;
}

JSValue native_drawStatusBar(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}
