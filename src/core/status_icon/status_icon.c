#include "core_sdk/status_icon.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/icon.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bruce_status_icon_t *s_icons;
static size_t s_count;
static uint32_t s_revision;

static bool status_icon__ensure_lock(void) {
    if (s_mutex != NULL) { return true; }
    taskENTER_CRITICAL(&s_init_lock);
    if (s_mutex == NULL) { s_mutex = xSemaphoreCreateMutex(); }
    taskEXIT_CRITICAL(&s_init_lock);
    return s_mutex != NULL;
}

static int status_icon__key_compare(const void *left, const void *right) {
    const bruce_status_icon_t *a = left;
    const bruce_status_icon_t *b = right;
    return strcmp(a->key, b->key);
}

bruce_result_t status_icon__push(const char *key, const uint8_t *bitmap, uint8_t width, uint8_t height) {
    if (key == NULL || bitmap == NULL || key[0] == '\0' || strlen(key) >= BRUCE_STATUS_ICON_KEY_MAX ||
        width == 0 || height == 0 || width > BRUCE_STATUS_ICON_MAX_WIDTH ||
        height > BRUCE_STATUS_ICON_MAX_HEIGHT) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    size_t bitmap_size = (size_t)((width + 7) / 8) * height;
    if (bitmap_size > BRUCE_STATUS_ICON_BITMAP_MAX || !status_icon__ensure_lock()) {
        return bitmap_size > BRUCE_STATUS_ICON_BITMAP_MAX ? BRUCE_ERR_INVALID_ARGUMENT : BRUCE_ERR_NO_MEMORY;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t index = s_count;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_icons[i].key, key) == 0) {
            index = i;
            break;
        }
    }
    if (index == s_count && s_count == BRUCE_STATUS_ICON_MAX) {
        xSemaphoreGive(s_mutex);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    if (index == s_count) {
        bruce_status_icon_t *grown = realloc(s_icons, (s_count + 1u) * sizeof(*s_icons));
        if (grown == NULL) {
            xSemaphoreGive(s_mutex);
            return BRUCE_ERR_NO_MEMORY;
        }
        s_icons = grown;
    }

    bruce_status_icon_t next = {0};
    strncpy(next.key, key, sizeof(next.key) - 1);
    next.width = width;
    next.height = height;
    memcpy(next.bitmap, bitmap, bitmap_size);
    if (index < s_count && memcmp(&s_icons[index], &next, sizeof(next)) == 0) {
        xSemaphoreGive(s_mutex);
        return BRUCE_OK;
    }
    s_icons[index] = next;
    if (index == s_count) { s_count++; }
    qsort(s_icons, s_count, sizeof(s_icons[0]), status_icon__key_compare);
    s_revision++;
    xSemaphoreGive(s_mutex);
    return BRUCE_OK;
}

bruce_result_t status_icon__push_named(const char *key, const char *icon_name) {
    const bruce_icon_t *icon = icon__get(icon_name);
    if (icon == NULL) return BRUCE_ERR_NOT_FOUND;
    if (icon->bits == NULL || icon->width == 0 || icon->height == 0) return BRUCE_ERR_INVALID_STATE;

    uint8_t width = icon->width;
    uint8_t height = icon->height;
    if (width > BRUCE_STATUS_ICON_MAX_WIDTH || height > BRUCE_STATUS_ICON_MAX_HEIGHT) {
        if (width >= height) {
            width = BRUCE_STATUS_ICON_MAX_WIDTH;
            height = (uint8_t)(((uint16_t)icon->height * width) / icon->width);
        } else {
            height = BRUCE_STATUS_ICON_MAX_HEIGHT;
            width = (uint8_t)(((uint16_t)icon->width * height) / icon->height);
        }
        if (width == 0) width = 1;
        if (height == 0) height = 1;
    }

    uint8_t bitmap[BRUCE_STATUS_ICON_BITMAP_MAX] = {0};
    size_t source_stride = (icon->width + 7u) / 8u;
    size_t target_stride = (width + 7u) / 8u;
    for (uint8_t y = 0; y < height; ++y) {
        uint8_t source_y = (uint8_t)(((uint16_t)(2u * y + 1u) * icon->height) / (2u * height));
        for (uint8_t x = 0; x < width; ++x) {
            uint8_t source_x = (uint8_t)(((uint16_t)(2u * x + 1u) * icon->width) / (2u * width));
            if ((icon->bits[(size_t)source_y * source_stride + source_x / 8u] &
                 (uint8_t)(0x80u >> (source_x % 8u))) != 0) {
                bitmap[(size_t)y * target_stride + x / 8u] |= (uint8_t)(0x80u >> (x % 8u));
            }
        }
    }
    return status_icon__push(key, bitmap, width, height);
}

bruce_result_t status_icon__remove(const char *key) {
    if (key == NULL || key[0] == '\0' || strlen(key) >= BRUCE_STATUS_ICON_KEY_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!status_icon__ensure_lock()) { return BRUCE_ERR_NO_MEMORY; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_icons[i].key, key) == 0) {
            if (i + 1 < s_count) {
                memmove(&s_icons[i], &s_icons[i + 1], (s_count - i - 1) * sizeof(s_icons[0]));
            }
            s_count--;
            if (s_count == 0) {
                free(s_icons);
                s_icons = NULL;
            } else {
                bruce_status_icon_t *shrunk = realloc(s_icons, s_count * sizeof(*s_icons));
                if (shrunk != NULL) s_icons = shrunk;
            }
            s_revision++;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return BRUCE_OK;
}

bruce_result_t
status_icon__list(bruce_status_icon_t *icons, size_t capacity, size_t *out_count, uint32_t *out_revision) {
    if ((capacity > 0 && icons == NULL) || out_count == NULL || out_revision == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!status_icon__ensure_lock()) { return BRUCE_ERR_NO_MEMORY; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t copied = capacity < s_count ? capacity : s_count;
    if (copied > 0) { memcpy(icons, s_icons, copied * sizeof(s_icons[0])); }
    *out_count = s_count;
    *out_revision = s_revision;
    xSemaphoreGive(s_mutex);
    return BRUCE_OK;
}
