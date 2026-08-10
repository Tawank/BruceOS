#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/loader.h"
#include "core_sdk/result.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    bruce_loader_image_t external;
    uint8_t *internal;
    bruce_result_t external_result;
    bool internal_tracked;
} js_source_t;

bruce_result_t js_source__load(const char *path, size_t max_size, js_source_t *out_source);
bruce_result_t js_source__load_transferable(const char *path, size_t max_size, js_source_t *out_source);
bruce_result_t js_source__adopt(js_source_t *source);
void js_source__release(js_source_t *source);
