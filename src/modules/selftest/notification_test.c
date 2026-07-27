#include "notification_test.h"

#include <stdio.h>
#include <string.h>

#include "core/display/display.h"
#include "core_sdk/display.h"
#include "core_sdk/notification.h"
#include "core_sdk/status_icon.h"

bool selftest__run_notification_case(void) {
    int width = display__width();
    int height = display__height();
    bruce_display_color_t before = 0;
    bruce_display_color_t after = 0;
    if (display__test_read_pixel(width - 3, height - 3, &before) != BRUCE_OK) return false;
    if (notification__push("first", 1) != BRUCE_OK ||
        notification__push("replacement", UINT32_MAX) != BRUCE_OK) {
        return false;
    }
    char text[BRUCE_NOTIFICATION_TEXT_MAX];
    bool active = false;
    uint32_t duration = 0;
    uint32_t generation = 0;
    bruce_display_rect_t rect = {0};
    if (display__test_notification(text, sizeof(text), &active, &duration, &rect, &generation) != BRUCE_OK ||
        !active || strcmp(text, "replacement") != 0 || duration != BRUCE_NOTIFICATION_DURATION_MAX_MS ||
        rect.x + rect.width != display__width() - 2 || rect.y + rect.height != display__height() - 2) {
        printf("[selftest] notification: state/copy/clamp/placement failed\n");
        return false;
    }
    if (display__test_read_pixel(width - 3, height - 3, &after) != BRUCE_OK || after != before) {
        printf("[selftest] notification: application framebuffer was modified\n");
        return false;
    }
    if (notification__dismiss() != BRUCE_OK || notification__dismiss() != BRUCE_OK) return false;
    uint32_t dismissed_generation = 0;
    if (display__test_notification(text, sizeof(text), &active, &duration, &rect, &dismissed_generation) !=
            BRUCE_OK ||
        active || dismissed_generation <= generation) {
        return false;
    }
    return true;
}

bool selftest__run_status_icon_case(void) {
    static const uint8_t bitmap_a[] = {0x80};
    static const uint8_t bitmap_b[] = {0xc0};
    (void)status_icon__remove("selftest-a");
    (void)status_icon__remove("selftest-b");
    size_t before_count = 0;
    uint32_t before_revision = 0;
    if (status_icon__list(NULL, 0, &before_count, &before_revision) != BRUCE_OK ||
        status_icon__push("selftest-b", bitmap_a, 1, 1) != BRUCE_OK ||
        status_icon__push("selftest-a", bitmap_a, 1, 1) != BRUCE_OK ||
        status_icon__push("selftest-a", bitmap_b, 2, 1) != BRUCE_OK) {
        return false;
    }
    bruce_status_icon_t icons[BRUCE_STATUS_ICON_MAX];
    size_t count = 0;
    uint32_t revision = 0;
    if (status_icon__list(icons, BRUCE_STATUS_ICON_MAX, &count, &revision) != BRUCE_OK ||
        count != before_count + 2 || revision < before_revision + 3) {
        return false;
    }
    size_t a = count;
    size_t b = count;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(icons[i].key, "selftest-a") == 0) a = i;
        if (strcmp(icons[i].key, "selftest-b") == 0) b = i;
    }
    bool valid = a < b && b < count && icons[a].width == 2 && icons[a].bitmap[0] == bitmap_b[0];
    valid = valid && status_icon__push("", bitmap_a, 1, 1) == BRUCE_ERR_INVALID_ARGUMENT;
    valid = valid && status_icon__remove("selftest-a") == BRUCE_OK;
    valid = valid && status_icon__remove("selftest-a") == BRUCE_OK;
    valid = valid && status_icon__remove("selftest-b") == BRUCE_OK;

    char keys[BRUCE_STATUS_ICON_MAX][BRUCE_STATUS_ICON_KEY_MAX];
    size_t inserted = 0;
    for (; inserted < BRUCE_STATUS_ICON_MAX - before_count; ++inserted) {
        snprintf(keys[inserted], sizeof(keys[inserted]), "selftest-fill-%02u", (unsigned)inserted);
        if (status_icon__push(keys[inserted], bitmap_a, 1, 1) != BRUCE_OK) {
            valid = false;
            break;
        }
    }
    if (valid && inserted > 0) {
        valid = status_icon__push(keys[0], bitmap_b, 2, 1) == BRUCE_OK;
        valid = valid && status_icon__push("selftest-overflow", bitmap_a, 1, 1) == BRUCE_ERR_RESOURCE_LIMIT;
    }
    for (size_t i = 0; i < inserted; ++i) { (void)status_icon__remove(keys[i]); }
    return valid;
}
