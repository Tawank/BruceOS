#include "js_bindings.h"
#include "js_bindings_private.h"

#include <stddef.h>

static const js_binding_t *s_modules[8];
static size_t s_module_counts[8];
static size_t s_module_count = 0;

void js_bindings__add_module(const js_binding_t *bindings, size_t count)
{
    if (s_module_count < sizeof(s_modules) / sizeof(s_modules[0])) {
        s_modules[s_module_count] = bindings;
        s_module_counts[s_module_count] = count;
        s_module_count++;
    }
}

static const js_binding_t *jsb_find_binding(const char *name)
{
    for (size_t m = 0; m < s_module_count; ++m) {
        const js_binding_t *bindings = s_modules[m];
        size_t count = s_module_counts[m];
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(bindings[i].name, name) == 0) {
                return &bindings[i];
            }
        }
    }
    return NULL;
}

JSValue js_bindings__dispatch(JSContext *ctx, const char *name, JSValue args_array)
{
    const js_binding_t *binding = jsb_find_binding(name);
    if (binding == NULL) {
        return JS_ThrowReferenceError(ctx, "unknown Bruce binding: %s", name);
    }

    JSGCRef args_ref;
    JSValue *args_ptr = NULL;
    if (JS_IsObject(ctx, args_array)) {
        args_ptr = JS_AddGCRef(ctx, &args_ref);
        *args_ptr = args_array;
    }

    JSValue result = binding->fn(ctx, args_ptr);

    if (args_ptr != NULL) {
        JS_DeleteGCRef(ctx, &args_ref);
    }
    return result;
}

void js_bindings__register(JSContext *ctx)
{
    jsb_runtime__init(ctx);
    jsb_serial__init(ctx);
    jsb_wifi__init(ctx);
    jsb_display__init(ctx);
    jsb_dialog__init(ctx);
}
