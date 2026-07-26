#include "core_sdk/status_icon.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bruce_status_icon_t s_icons[BRUCE_STATUS_ICON_MAX];
static size_t s_count;
static uint32_t s_revision;

static bool status_icon__ensure_lock(void)
{
    if (s_mutex != NULL) {
        return true;
    }
    taskENTER_CRITICAL(&s_init_lock);
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    taskEXIT_CRITICAL(&s_init_lock);
    return s_mutex != NULL;
}

static int status_icon__key_compare(const void *left, const void *right)
{
    const bruce_status_icon_t *a = left;
    const bruce_status_icon_t *b = right;
    return strcmp(a->key, b->key);
}

bruce_result_t status_icon__push(const char *key, const uint8_t *bitmap,
                                  uint8_t width, uint8_t height)
{
    if (key == NULL || bitmap == NULL || key[0] == '\0' ||
        strlen(key) >= BRUCE_STATUS_ICON_KEY_MAX || width == 0 || height == 0 ||
        width > BRUCE_STATUS_ICON_MAX_WIDTH || height > BRUCE_STATUS_ICON_MAX_HEIGHT) {
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
    if (index == s_count) {
        s_count++;
    }
    qsort(s_icons, s_count, sizeof(s_icons[0]), status_icon__key_compare);
    s_revision++;
    xSemaphoreGive(s_mutex);
    return BRUCE_OK;
}

bruce_result_t status_icon__remove(const char *key)
{
    if (key == NULL || key[0] == '\0' || strlen(key) >= BRUCE_STATUS_ICON_KEY_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!status_icon__ensure_lock()) {
        return BRUCE_ERR_NO_MEMORY;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_icons[i].key, key) == 0) {
            if (i + 1 < s_count) {
                memmove(&s_icons[i], &s_icons[i + 1], (s_count - i - 1) * sizeof(s_icons[0]));
            }
            memset(&s_icons[--s_count], 0, sizeof(s_icons[0]));
            s_revision++;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return BRUCE_OK;
}

bruce_result_t status_icon__list(bruce_status_icon_t *icons, size_t capacity,
                                  size_t *out_count, uint32_t *out_revision)
{
    if ((capacity > 0 && icons == NULL) || out_count == NULL || out_revision == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!status_icon__ensure_lock()) {
        return BRUCE_ERR_NO_MEMORY;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t copied = capacity < s_count ? capacity : s_count;
    if (copied > 0) {
        memcpy(icons, s_icons, copied * sizeof(s_icons[0]));
    }
    *out_count = s_count;
    *out_revision = s_revision;
    xSemaphoreGive(s_mutex);
    return BRUCE_OK;
}
