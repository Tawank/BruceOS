#include "notification_test.h"

#include <stdio.h>
#include <string.h>

#include "core/display/display.h"
#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/notification.h"
#include "core_sdk/runtime.h"
#include "core_sdk/status_icon.h"
#include "modules/notification_service/notification_service.h"

#define NOTIFICATION_TEST__POLL_TIMEOUT_MS 2000u
#define NOTIFICATION_TEST__POLL_STEP_MS 10u

/* modules/notification_service/notification_service.c renders asynchronously: it is
 * a separate background process woken by notification__push()/dismiss(),
 * so its effect on the overlay is not visible the instant those calls
 * return. Polls until the service's overlay reaches the wanted visibility,
 * or times out. */
static bool notification_test__wait_visible(bool want_visible, bruce_display_rect_t *out_rect) {
    for (uint32_t waited = 0; waited <= NOTIFICATION_TEST__POLL_TIMEOUT_MS;
         waited += NOTIFICATION_TEST__POLL_STEP_MS) {
        bruce_display_overlay_id_t overlay = notification_service__test_overlay_id();
        if (overlay != BRUCE_DISPLAY_OVERLAY_ID_INVALID) {
            bruce_display_rect_t rect = {0};
            bool visible = false;
            uint32_t generation = 0;
            if (display__test_overlay_state(overlay, &rect, &visible, &generation) == BRUCE_OK &&
                visible == want_visible) {
                if (out_rect != NULL) *out_rect = rect;
                return true;
            }
        }
        (void)runtime__delay(NOTIFICATION_TEST__POLL_STEP_MS);
    }
    return false;
}

bool selftest__run_notification_case(void) {
    int width = display__width();
    int height = display__height();
    bruce_display_color_t before = 0;
    bruce_display_color_t after = 0;
    bool buffered = config__get_display_buffered_rendering();
    if (buffered && display__test_read_pixel(width - 3, height - 3, &before) != BRUCE_OK) return false;

    if (notification__push("first", 1) != BRUCE_OK || notification__push("replacement", UINT32_MAX) != BRUCE_OK) {
        printf("[selftest] notification: push failed\n");
        return false;
    }
    bruce_display_rect_t rect = {0};
    if (!notification_test__wait_visible(true, &rect)) {
        printf("[selftest] notification: service never showed the overlay\n");
        return false;
    }
    /* "first" (duration clamped to the public minimum) must have been fully
     * replaced by "replacement" (UINT32_MAX clamped to the public maximum)
     * without the service ever rendering the earlier one on its own overlay
     * -- last-writer-wins, exactly like notification__push() documents. */
    if (rect.x + rect.width != display__width() - 2 || rect.y + rect.height != display__height() - 2) {
        printf("[selftest] notification: placement failed\n");
        return false;
    }
    bruce_display_overlay_id_t overlay = notification_service__test_overlay_id();
    bruce_display_color_t border = 0;
    bool saw_glyph_pixel = false;
    if (display__test_overlay_pixel(overlay, 0, 0, &border) != BRUCE_OK || border != BRUCE_COLOR_WHITE) {
        printf("[selftest] notification: border color failed\n");
        return false;
    }
    for (int16_t y = 4; y < 11 && !saw_glyph_pixel; ++y) {
        for (int16_t x = 4; x < rect.width - 3 && !saw_glyph_pixel; ++x) {
            bruce_display_color_t pixel = 0;
            if (display__test_overlay_pixel(overlay, x, y, &pixel) == BRUCE_OK && pixel == BRUCE_COLOR_WHITE) {
                saw_glyph_pixel = true;
            }
        }
    }
    if (!saw_glyph_pixel) {
        printf("[selftest] notification: no text pixels found\n");
        return false;
    }
    if (buffered && (display__test_read_pixel(width - 3, height - 3, &after) != BRUCE_OK || after != before)) {
        printf("[selftest] notification: application framebuffer was modified\n");
        return false;
    }

    if (notification__dismiss() != BRUCE_OK || notification__dismiss() != BRUCE_OK) return false;
    if (!notification_test__wait_visible(false, NULL)) {
        printf("[selftest] notification: service never hid the overlay\n");
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
