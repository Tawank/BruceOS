#include "notification_test.h"

#include <stdio.h>
#include <string.h>

#include "core/display/display.h"
#include "core/process/process.h"
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

/* notification__push() decides GUI-banner-vs-console by reading the
 * *calling* process's own GUI=1 flag (see process_registry__current_context()
 * in core/notification/notification.c). The selftest runner itself is
 * launched in the background without GUI=1 (see app_runner__run_command()
 * for "selftest"), so pushing directly from it would always exercise the
 * console fallback rather than whichever path a given case wants to test.
 * These helpers spawn a short-lived child with an explicit gui_requested
 * flag to push from, mirroring the pattern
 * selftest__run_dialog_dispatch_as() (permission_test.c) already uses for
 * the analogous dialog__* GUI/terminal dispatch check. */
static bool s_push_ran;
static bool s_push_ok;

static int selftest__notification_push_pair_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_push_ok =
        notification__push("first", 1) == BRUCE_OK && notification__push("replacement", UINT32_MAX) == BRUCE_OK;
    s_push_ran = true;
    return 0;
}

static int selftest__notification_push_one_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_push_ok = notification__push("console only", 1) == BRUCE_OK;
    s_push_ran = true;
    return 0;
}

static bool selftest__push_notifications_as(bool gui_requested, bruce_app_entry_t entry) {
    s_push_ran = false;
    s_push_ok = false;
    process_create_params_t params = {
        .name = "selftest_notification_push",
        .entry = entry,
        .argc = 0,
        .argv = NULL,
        .built_in = false,
        .gui_requested = gui_requested,
        .permission_key = "",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) return false;
    bruce_result_t wait_result = process__wait(id, 2000);
    return (wait_result == BRUCE_OK || wait_result == BRUCE_ERR_NOT_FOUND) && s_push_ran && s_push_ok;
}

bool selftest__run_notification_case(void) {
    int width = display__width();
    int height = display__height();
    bruce_display_color_t before = 0;
    bruce_display_color_t after = 0;
    bool buffered = config__get_display_buffered_rendering();
    if (buffered && display__test_read_pixel(width - 3, height - 3, &before) != BRUCE_OK) return false;

    if (!selftest__push_notifications_as(true, selftest__notification_push_pair_entry)) {
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

/* Companion to selftest__run_notification_case(): pushes from a process
 * launched *without* GUI interaction and asserts the service never draws a
 * banner for it -- the console-fallback branch (modules/notification_service
 * printing via stdio__printf() instead of display__overlay_*) is exercised
 * here by omission: if it ever regressed back to always drawing an overlay,
 * this would start failing. */
bool selftest__run_notification_console_fallback_case(void) {
    if (!selftest__push_notifications_as(false, selftest__notification_push_one_entry)) {
        printf("[selftest] notification/console-fallback: push failed\n");
        return false;
    }
    if (notification_test__wait_visible(true, NULL)) {
        printf("[selftest] notification/console-fallback: overlay shown for a non-GUI push\n");
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
