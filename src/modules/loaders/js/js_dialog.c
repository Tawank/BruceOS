#include "js_bindings_private.h"

#include "core_sdk/dialog.h"
#include "core_sdk/storage.h"

static JSValue jsb_dialog_message(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *msg = jsb_arg_string(ctx, args_ptr, 0, &buf);
    if (msg == NULL) {
        return JS_ThrowTypeError(ctx, "dialog.message(msg:string, buttons?:object)");
    }

    bool has_buttons = jsb_arg_is_object(ctx, args_ptr, 1);
    if (!has_buttons) {
        dialog__message(BRUCE_DIALOG_INFO, NULL, msg);
        return JS_NewString(ctx, "right");
    }

    JSValue buttons = jsb_get_arg(ctx, args_ptr, 1);
    bruce_dialog_choice_t choices[3];
    size_t choice_count = 0;
    JSCStringBuf left_buf, center_buf, right_buf;
    const char *left = NULL;
    const char *center = NULL;
    const char *right = NULL;

    JSValue v = JS_GetPropertyStr(ctx, buttons, "left");
    if (JS_IsString(ctx, v)) {
        left = JS_ToCString(ctx, v, &left_buf);
        choices[choice_count].label = left;
        choices[choice_count].value = left;
        choice_count++;
    }
    v = JS_GetPropertyStr(ctx, buttons, "center");
    if (JS_IsString(ctx, v)) {
        center = JS_ToCString(ctx, v, &center_buf);
        choices[choice_count].label = center;
        choices[choice_count].value = center;
        choice_count++;
    }
    v = JS_GetPropertyStr(ctx, buttons, "right");
    if (JS_IsString(ctx, v)) {
        right = JS_ToCString(ctx, v, &right_buf);
        choices[choice_count].label = right;
        choices[choice_count].value = right;
        choice_count++;
    }

    size_t selected = 0;
    bruce_result_t result = dialog__choice(NULL, msg, choices, choice_count, &selected);
    if (result != BRUCE_OK) {
        return JS_NewString(ctx, "");
    }
    const char *rv = "";
    if (selected == 0 && left != NULL) rv = left;
    else if (selected == 1 && center != NULL) rv = center;
    else if (right != NULL) rv = right;
    return JS_NewString(ctx, rv);
}

static JSValue jsb_dialog_info(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *s = jsb_arg_string(ctx, args_ptr, 0, &buf);
    if (s != NULL) {
        dialog__message(BRUCE_DIALOG_INFO, NULL, s);
    }
    return JS_UNDEFINED;
}

static JSValue jsb_dialog_success(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *s = jsb_arg_string(ctx, args_ptr, 0, &buf);
    if (s != NULL) {
        dialog__message(BRUCE_DIALOG_SUCCESS, NULL, s);
    }
    return JS_UNDEFINED;
}

static JSValue jsb_dialog_warning(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *s = jsb_arg_string(ctx, args_ptr, 0, &buf);
    if (s != NULL) {
        dialog__message(BRUCE_DIALOG_WARNING, NULL, s);
    }
    return JS_UNDEFINED;
}

static JSValue jsb_dialog_error(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *s = jsb_arg_string(ctx, args_ptr, 0, &buf);
    if (s != NULL) {
        dialog__message(BRUCE_DIALOG_ERROR, NULL, s);
    }
    return JS_UNDEFINED;
}

static JSValue jsb_dialog_pickFile(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf path_buf, ext_buf;
    const char *path = jsb_arg_string(ctx, args_ptr, 0, &path_buf);
    const char *ext = jsb_arg_string(ctx, args_ptr, 1, &ext_buf);
    char out[BRUCE_STORAGE_PATH_MAX];
    out[0] = '\0';
    bruce_result_t result = dialog__pick_file(path != NULL ? path : "/",
                                               ext != NULL ? ext : "*", out, sizeof(out));
    if (result != BRUCE_OK) {
        return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, out);
}

static JSValue jsb_dialog_choice(JSContext *ctx, JSValue *args_ptr)
{
    if (!jsb_arg_is_object(ctx, args_ptr, 0)) {
        return JS_ThrowTypeError(ctx, "dialog.choice(choices: object|array)");
    }

    JSValue choices_in = jsb_get_arg(ctx, args_ptr, 0);
    bruce_dialog_choice_t choices[32];
    size_t choice_count = 0;
    char value_storage[32][64];

    if (jsb_arg_is_array(ctx, args_ptr, 0)) {
        uint32_t len = jsb_array_length(ctx, choices_in);
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
            if (key == NULL) break;
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
    bruce_result_t result = dialog__choice(NULL, NULL, choices, choice_count, &selected);
    if (result != BRUCE_OK || selected >= choice_count) {
        return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, choices[selected].value);
}

static JSValue jsb_dialog_viewFile(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    /* dialog.viewFile() is intentionally not migrated; use createTextViewer(). */
    return JS_UNDEFINED;
}

static JSValue jsb_dialog_viewText(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *text = jsb_arg_string(ctx, args_ptr, 0, &buf);
    if (text == NULL) {
        return JS_ThrowTypeError(ctx, "dialog.viewText(text:string, title?:string)");
    }
    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    dialog__create_text_viewer(NULL, text, &viewer);
    return JS_NewUint32(ctx, viewer);
}

static JSValue jsb_dialog_createTextViewer(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf buf;
    const char *text = jsb_arg_string(ctx, args_ptr, 0, &buf);
    if (text == NULL) {
        return JS_ThrowTypeError(ctx, "dialog.createTextViewer(text:string)");
    }
    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    dialog__create_text_viewer(NULL, text, &viewer);
    return JS_NewUint32(ctx, viewer);
}

static JSValue jsb_dialog_textInput(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf t, p, i;
    const char *title = jsb_arg_string(ctx, args_ptr, 0, &t);
    const char *prompt = jsb_arg_string(ctx, args_ptr, 1, &p);
    const char *initial = jsb_arg_string(ctx, args_ptr, 2, &i);
    int mask = 0;
    jsb_arg_bool(ctx, args_ptr, 3, &mask);
    char buffer[256];
    buffer[0] = '\0';
    bruce_result_t result = dialog__text_input(title, prompt, initial, mask != 0, buffer, sizeof(buffer));
    if (result != BRUCE_OK) {
        return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, buffer);
}

static JSValue jsb_dialog_hexInput(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf t, p, i;
    const char *title = jsb_arg_string(ctx, args_ptr, 0, &t);
    const char *prompt = jsb_arg_string(ctx, args_ptr, 1, &p);
    const char *initial = jsb_arg_string(ctx, args_ptr, 2, &i);
    char buffer[256];
    buffer[0] = '\0';
    bruce_result_t result = dialog__hex_input(title, prompt, initial, buffer, sizeof(buffer));
    if (result != BRUCE_OK) {
        return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, buffer);
}

static JSValue jsb_dialog_numberInput(JSContext *ctx, JSValue *args_ptr)
{
    JSCStringBuf t, p, i;
    const char *title = jsb_arg_string(ctx, args_ptr, 0, &t);
    const char *prompt = jsb_arg_string(ctx, args_ptr, 1, &p);
    const char *initial = jsb_arg_string(ctx, args_ptr, 2, &i);
    char buffer[64];
    buffer[0] = '\0';
    bruce_result_t result = dialog__number_input(title, prompt, initial, buffer, sizeof(buffer));
    if (result != BRUCE_OK) {
        return JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, buffer);
}

static JSValue jsb_dialog_drawStatusBar(JSContext *ctx, JSValue *args_ptr)
{
    (void)ctx;
    (void)args_ptr;
    return JS_UNDEFINED;
}

static const js_binding_t s_dialog_bindings[] = {
    {"dialog.message", jsb_dialog_message},
    {"dialog.info", jsb_dialog_info},
    {"dialog.success", jsb_dialog_success},
    {"dialog.warning", jsb_dialog_warning},
    {"dialog.error", jsb_dialog_error},
    {"dialog.pickFile", jsb_dialog_pickFile},
    {"dialog.choice", jsb_dialog_choice},
    {"dialog.viewFile", jsb_dialog_viewFile},
    {"dialog.viewText", jsb_dialog_viewText},
    {"dialog.createTextViewer", jsb_dialog_createTextViewer},
    {"dialog.textInput", jsb_dialog_textInput},
    {"dialog.hexInput", jsb_dialog_hexInput},
    {"dialog.numberInput", jsb_dialog_numberInput},
    {"dialog.drawStatusBar", jsb_dialog_drawStatusBar},
};

static const char s_dialog_setup[] =
    "(function(){"
    "var bridge=globalThis.__bruce_bridge;"
    "if(typeof bridge!=='function')return;"
    "globalThis.dialog=globalThis.dialog||{};"
    "var methods=['message','info','success','warning','error','pickFile','choice','viewFile','viewText','createTextViewer','textInput','hexInput','numberInput','drawStatusBar'];"
    "for(var i=0;i<methods.length;i++){"
    "(function(full,method){"
    "globalThis.dialog[method]=function(){return bridge(full,Array.prototype.slice.call(arguments));};"
    "})('dialog.'+methods[i],methods[i]);"
    "}"
    "})();";

void jsb_dialog__init(JSContext *ctx)
{
    js_bindings__add_module(s_dialog_bindings,
                            sizeof(s_dialog_bindings) / sizeof(s_dialog_bindings[0]));
    JSValue result = JS_Eval(ctx, s_dialog_setup, sizeof(s_dialog_setup) - 1, "js_dialog_setup", 0);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        printf("[js_dialog] setup error: ");
        JS_PrintValueF(ctx, ex, JS_DUMP_LONG);
        printf("\n");
    }
}
