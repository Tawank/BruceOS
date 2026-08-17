#include "notification_service.h"

#include <string.h>

#include "core_sdk/display.h"
#include "core_sdk/notification.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define NOTIFICATION_SERVICE__PADDING 8
#define NOTIFICATION_SERVICE__MIN_WIDTH 20
#define NOTIFICATION_SERVICE__SCREEN_MARGIN 2
#define NOTIFICATION_SERVICE__BOTTOM_GAP 10

static bruce_display_overlay_id_t s_overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
static int16_t s_overlay_width;
static int16_t s_overlay_height;

bruce_display_overlay_id_t notification_service__test_overlay_id(void) { return s_overlay; }

static void notification_service__hide(void) {
    if (s_overlay != BRUCE_DISPLAY_OVERLAY_ID_INVALID) (void)display__overlay_hide(s_overlay);
}

/* Recomputes the banner's rect for `text` and (re)draws it into the
 * service's overlay, creating or resizing that overlay as needed. The
 * overlay is destroyed and recreated on a width change rather than resized
 * in place -- display__overlay_* has no resize call, and pushes are rare
 * enough that this costs nothing that matters. */
static bruce_result_t notification_service__show(const char *text) {
    int screen_w = display__screen_width();
    int screen_h = display__screen_height();
    if (screen_w <= 0 || screen_h <= 0) return BRUCE_ERR_NOT_INITIALIZED;

    int16_t char_w = 0;
    int16_t char_h = 0;
    (void)display__get_font_metrics(&char_w, &char_h);

    int width = (int)strlen(text) * char_w + NOTIFICATION_SERVICE__PADDING;
    if (width > screen_w - 4) width = screen_w - 4;
    if (width < NOTIFICATION_SERVICE__MIN_WIDTH) width = NOTIFICATION_SERVICE__MIN_WIDTH;
    int height = char_h + 8;
    int16_t x = (int16_t)(screen_w - width - NOTIFICATION_SERVICE__SCREEN_MARGIN);
    int16_t y = (int16_t)(screen_h - char_h - NOTIFICATION_SERVICE__BOTTOM_GAP);

    if (s_overlay != BRUCE_DISPLAY_OVERLAY_ID_INVALID &&
        (s_overlay_width != width || s_overlay_height != height)) {
        (void)display__overlay_destroy(s_overlay);
        s_overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
    }
    if (s_overlay == BRUCE_DISPLAY_OVERLAY_ID_INVALID) {
        bruce_result_t result =
            display__overlay_create(x, y, (int16_t)width, (int16_t)height, &s_overlay);
        if (result != BRUCE_OK) return result;
        s_overlay_width = (int16_t)width;
        s_overlay_height = (int16_t)height;
    } else {
        (void)display__overlay_move(s_overlay, x, y);
    }

    bruce_result_t result = display__overlay_begin(s_overlay);
    if (result != BRUCE_OK) return result;
    (void)display__fill_rect(0, 0, (int16_t)width, (int16_t)height, BRUCE_COLOR_NAVY);
    (void)display__draw_rect(0, 0, (int16_t)width, (int16_t)height, BRUCE_COLOR_WHITE);
    (void)display__set_text_size(1);
    (void)display__set_text_color(BRUCE_COLOR_WHITE);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__draw_string(text, 4, 4);
    (void)display__overlay_end(s_overlay);
    return display__overlay_show(s_overlay);
}

int notification_service_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool visible = false;
    uint64_t deadline_ms = 0;
    for (;;) {
        char text[BRUCE_NOTIFICATION_TEXT_MAX];
        uint32_t duration_ms = 0;
        bool dismiss = false;
        bool gui_requested = false;
        bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
        uint32_t wait_ms = UINT32_MAX;
        if (visible) {
            uint64_t now = runtime__now();
            wait_ms = deadline_ms > now ? (uint32_t)(deadline_ms - now) : 0;
        }
        bruce_result_t result = notification__wait_request(
            text, sizeof(text), &duration_ms, &dismiss, &gui_requested, &session, wait_ms
        );
        if (result == BRUCE_OK && dismiss) {
            notification_service__hide();
            visible = false;
        } else if (result == BRUCE_OK && gui_requested) {
            if (notification_service__show(text) == BRUCE_OK) {
                visible = true;
                deadline_ms = runtime__now() + duration_ms;
            }
        } else if (result == BRUCE_OK) {
            /* Pushed by a process launched without GUI interaction (a typed
             * console command): print straight to its routed stdio session
             * instead of drawing a banner nobody watching the console would
             * see. That has to be the *pusher's* session, captured at push
             * time (see notification__push()/notification__wait_request()) --
             * this service is its own background process, so stdio__printf()
             * here would route to this process's own (normally absent)
             * session instead, landing on the physical serial console no
             * matter which session actually pushed the notification. */
            (void)stdio__write_to(session, text, strlen(text));
            (void)stdio__write_to(session, "\n", 1);
        } else if (visible) {
            /* Timed out waiting for the next request: this notification's
             * duration has elapsed with nothing new queued behind it. */
            notification_service__hide();
            visible = false;
        }
    }
}
