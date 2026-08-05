#pragma once

#include "core_sdk/display.h"

/* Background service: consumes notification__wait_request() (see
 * core_sdk/notification.h) and renders the transient notification banner
 * using only the public display__overlay_* primitive -- Core itself knows
 * nothing about what a notification looks like. Registered as a startup
 * app (see src/main.c, src/core/config/config.c); never returns. */
int notification_service_main(int argc, char **argv);

/* Selftest introspection seam: the overlay handle the service is currently
 * using, or BRUCE_DISPLAY_OVERLAY_ID_INVALID before its first notification. */
bruce_display_overlay_id_t notification_service__test_overlay_id(void);
