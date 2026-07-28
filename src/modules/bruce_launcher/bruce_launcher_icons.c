#include "bruce_launcher_icons.h"

#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

#include "core_sdk/display.h"
#include "core_sdk/icon.h"

typedef enum {
    BRUCE_LAUNCHER_ICON_FOLDER,
    BRUCE_LAUNCHER_ICON_WIFI,
    BRUCE_LAUNCHER_ICON_BLUETOOTH,
    BRUCE_LAUNCHER_ICON_IR,
    BRUCE_LAUNCHER_ICON_NRF24,
    BRUCE_LAUNCHER_ICON_FILES,
    BRUCE_LAUNCHER_ICON_TERMINAL,
    BRUCE_LAUNCHER_ICON_APPS,
    BRUCE_LAUNCHER_ICON_CONFIG,
    BRUCE_LAUNCHER_ICON_CLOCK,
    BRUCE_LAUNCHER_ICON_SELFTEST,
} bruce_launcher_icon_t;

static bruce_launcher_icon_t bruce_launcher__entry_icon(const bruce_launcher_entry_t *entry) {
    if (strcasecmp(entry->label, "wifi") == 0) return BRUCE_LAUNCHER_ICON_WIFI;
    if (strcasecmp(entry->label, "bluetooth") == 0) return BRUCE_LAUNCHER_ICON_BLUETOOTH;
    if (strcasecmp(entry->label, "infrared") == 0 || strcasecmp(entry->label, "ir") == 0) {
        return BRUCE_LAUNCHER_ICON_IR;
    }
    if (strcasecmp(entry->label, "nrf24") == 0) return BRUCE_LAUNCHER_ICON_NRF24;
    if (strcasecmp(entry->label, "files") == 0 || strcasecmp(entry->label, "file manager") == 0) {
        return BRUCE_LAUNCHER_ICON_FILES;
    }
    if (strcasecmp(entry->label, "terminal") == 0) return BRUCE_LAUNCHER_ICON_TERMINAL;
    if (strcasecmp(entry->label, "apps") == 0) return BRUCE_LAUNCHER_ICON_APPS;
    if (strcasecmp(entry->label, "config") == 0) return BRUCE_LAUNCHER_ICON_CONFIG;
    if (strcasecmp(entry->label, "clock") == 0) return BRUCE_LAUNCHER_ICON_CLOCK;
    if (strcasecmp(entry->label, "selftest") == 0 || strcasecmp(entry->label, "self-test") == 0) {
        return BRUCE_LAUNCHER_ICON_SELFTEST;
    }
    return entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU ? BRUCE_LAUNCHER_ICON_FOLDER
                                                       : BRUCE_LAUNCHER_ICON_APPS;
}

static void
bruce_launcher__draw_icon_path(int cx, int cy, int size, const char *name, bruce_display_color_t color) {
    const bruce_icon_t *icon = icon__get(name);
    if (icon == NULL) { return; }
    int x = cx - size / 2;
    int y = cy - size / 2;
    display__draw_bitmap_scaled(
        (int16_t)x, (int16_t)y, icon->bits, icon->width, icon->height, (int16_t)size, (int16_t)size, color
    );
}

void bruce_launcher__draw_entry_icon(
    const bruce_launcher_entry_t *entry, int cx, int cy, int size, uint16_t color
) {
    bruce_launcher_icon_t icon = bruce_launcher__entry_icon(entry);

    switch (icon) {
        case BRUCE_LAUNCHER_ICON_WIFI: {
            bruce_launcher__draw_icon_path(cx, cy, size, "wifi", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_BLUETOOTH: {
            bruce_launcher__draw_icon_path(cx, cy, size, "ble", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_NRF24: {
            bruce_launcher__draw_icon_path(cx, cy, size, "handheld", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_IR: {
            bruce_launcher__draw_icon_path(cx, cy, size, "remote", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_FILES: {
            bruce_launcher__draw_icon_path(cx, cy, size, "folder", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_TERMINAL: {
            bruce_launcher__draw_icon_path(cx, cy, size, "terminal", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_APPS: {
            bruce_launcher__draw_icon_path(cx, cy, size, "apps", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_CONFIG: {
            bruce_launcher__draw_icon_path(cx, cy, size, "settings", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_CLOCK: {
            bruce_launcher__draw_icon_path(cx, cy, size, "clock", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_SELFTEST: {
            bruce_launcher__draw_icon_path(cx, cy, size, "selftest", color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_FOLDER: {
            bruce_launcher__draw_icon_path(cx, cy, size, "folder", color);
            break;
        }
    }
}
