#include "bruce_launcher_icons.h"

#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

#include "core_sdk/display.h"

typedef enum {
    BRUCE_LAUNCHER_ICON_COMMAND,
    BRUCE_LAUNCHER_ICON_FOLDER,
    BRUCE_LAUNCHER_ICON_WIFI,
    BRUCE_LAUNCHER_ICON_WEBUI,
    BRUCE_LAUNCHER_ICON_BLUETOOTH,
    BRUCE_LAUNCHER_ICON_BLUETOOTH_HID,
    BRUCE_LAUNCHER_ICON_IR,
    BRUCE_LAUNCHER_ICON_NRF24,
    BRUCE_LAUNCHER_ICON_FILES,
    BRUCE_LAUNCHER_ICON_TERMINAL,
    BRUCE_LAUNCHER_ICON_TCP,
    BRUCE_LAUNCHER_ICON_NOTIFICATION,
    BRUCE_LAUNCHER_ICON_APPS,
    BRUCE_LAUNCHER_ICON_CONFIG,
    BRUCE_LAUNCHER_ICON_CLOCK,
    BRUCE_LAUNCHER_ICON_SELFTEST,
    BRUCE_LAUNCHER_ICON_BACK,
} bruce_launcher_icon_t;

static bruce_launcher_icon_t bruce_launcher__entry_icon(const bruce_launcher_entry_t *entry) {
    if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) return BRUCE_LAUNCHER_ICON_BACK;
    if (strcasecmp(entry->label, "wifi") == 0) return BRUCE_LAUNCHER_ICON_WIFI;
    if (strcasecmp(entry->label, "webui") == 0) return BRUCE_LAUNCHER_ICON_WEBUI;
    if (strcasecmp(entry->label, "bluetooth") == 0) return BRUCE_LAUNCHER_ICON_BLUETOOTH;
    if (strcasecmp(entry->label, "bluetooth hid") == 0) return BRUCE_LAUNCHER_ICON_BLUETOOTH_HID;
    if (strcasecmp(entry->label, "infrared") == 0 || strcasecmp(entry->label, "ir") == 0) {
        return BRUCE_LAUNCHER_ICON_IR;
    }
    if (strcasecmp(entry->label, "nrf24") == 0) return BRUCE_LAUNCHER_ICON_NRF24;
    if (strcasecmp(entry->label, "files") == 0 || strcasecmp(entry->label, "file manager") == 0) {
        return BRUCE_LAUNCHER_ICON_FILES;
    }
    if (strcasecmp(entry->label, "terminal") == 0) return BRUCE_LAUNCHER_ICON_TERMINAL;
    if (strcasecmp(entry->label, "tcp") == 0 || strcasecmp(entry->label, "tcp help") == 0) {
        return BRUCE_LAUNCHER_ICON_TCP;
    }
    if (strcasecmp(entry->label, "notifications") == 0 ||
        strcasecmp(entry->label, "notification demo") == 0) {
        return BRUCE_LAUNCHER_ICON_NOTIFICATION;
    }
    if (strcasecmp(entry->label, "apps") == 0) return BRUCE_LAUNCHER_ICON_APPS;
    if (strcasecmp(entry->label, "config") == 0) return BRUCE_LAUNCHER_ICON_CONFIG;
    if (strcasecmp(entry->label, "clock") == 0) return BRUCE_LAUNCHER_ICON_CLOCK;
    if (strcasecmp(entry->label, "selftest") == 0 || strcasecmp(entry->label, "self-test") == 0) {
        return BRUCE_LAUNCHER_ICON_SELFTEST;
    }
    return entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU ? BRUCE_LAUNCHER_ICON_FOLDER
                                                       : BRUCE_LAUNCHER_ICON_COMMAND;
}

static void bruce_launcher__draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
    int half = thickness / 2;
    bool mostly_horizontal = abs(x1 - x0) >= abs(y1 - y0);
    for (int offset = -half; offset <= half; ++offset) {
        display__draw_line(
            x0 + (mostly_horizontal ? 0 : offset),
            y0 + (mostly_horizontal ? offset : 0),
            x1 + (mostly_horizontal ? 0 : offset),
            y1 + (mostly_horizontal ? offset : 0),
            color
        );
    }
}

static void bruce_launcher__draw_arc_band(
    int cx, int cy, int outer_radius, int inner_radius, int start_angle, int end_angle, uint16_t color
) {
    if (inner_radius < 1) inner_radius = 1;
    for (int radius = inner_radius; radius <= outer_radius; ++radius) {
        display__draw_arc(cx, cy, radius, start_angle, end_angle, color);
    }
}

static void bruce_launcher__draw_box(int x, int y, int w, int h, int stroke, uint16_t color) {
    for (int inset = 0; inset < stroke && w - 2 * inset > 0 && h - 2 * inset > 0; ++inset) {
        display__draw_rect(x + inset, y + inset, w - 2 * inset, h - 2 * inset, color);
    }
}

static void bruce_launcher__draw_bluetooth_rune(int cx, int cy, int size, int stroke, uint16_t color) {
    int half_h = size * 3 / 8;
    int arm = size / 5;
    bruce_launcher__draw_thick_line(cx, cy - half_h, cx, cy + half_h, stroke, color);
    bruce_launcher__draw_thick_line(cx, cy - half_h, cx + arm, cy - arm / 2, stroke, color);
    bruce_launcher__draw_thick_line(cx + arm, cy - arm / 2, cx - arm / 2, cy + arm / 2, stroke, color);
    bruce_launcher__draw_thick_line(cx - arm / 2, cy - arm / 2, cx + arm, cy + arm / 2, stroke, color);
    bruce_launcher__draw_thick_line(cx + arm, cy + arm / 2, cx, cy + half_h, stroke, color);
}

static void bruce_launcher__draw_document(int cx, int cy, int size, int stroke, uint16_t color) {
    int page_w = size * 40 / 64;
    int page_h = size * 60 / 64;
    int fold = page_h / 4;
    int x = cx - page_w / 2;
    int y = cy - page_h / 2;
    bruce_launcher__draw_thick_line(x, y, x + page_w - fold, y, stroke, color);
    bruce_launcher__draw_thick_line(x + page_w - fold, y, x + page_w, y + fold, stroke, color);
    bruce_launcher__draw_thick_line(x + page_w, y + fold, x + page_w, y + page_h, stroke, color);
    bruce_launcher__draw_thick_line(x + page_w, y + page_h, x, y + page_h, stroke, color);
    bruce_launcher__draw_thick_line(x, y + page_h, x, y, stroke, color);
    bruce_launcher__draw_thick_line(x + page_w - fold, y, x + page_w - fold, y + fold, stroke, color);
    bruce_launcher__draw_thick_line(x + page_w - fold, y + fold, x + page_w, y + fold, stroke, color);
}

void bruce_launcher__draw_entry_icon(
    const bruce_launcher_entry_t *entry, int cx, int cy, int size, uint16_t color
) {
    bruce_launcher_icon_t icon = bruce_launcher__entry_icon(entry);
    int left = cx - size / 2;
    int top = cy - size / 2;
    int pad = size >= 48 ? 5 : 3;
    int stroke = size >= 48 ? 3 : 1;

    switch (icon) {
        case BRUCE_LAUNCHER_ICON_WIFI: {
            int delta_y = size * 20 / 64;
            int dot_radius = size * 6 / 64;
            int origin_y = cy + delta_y;
            display__fill_circle(cx, origin_y, dot_radius, color);
            bruce_launcher__draw_arc_band(cx, origin_y, delta_y + dot_radius, delta_y, 130, 230, color);
            bruce_launcher__draw_arc_band(
                cx, origin_y, 2 * delta_y + dot_radius, 2 * delta_y, 130, 230, color
            );
            break;
        }
        case BRUCE_LAUNCHER_ICON_WEBUI: {
            int radius = size * 3 / 8;
            bruce_launcher__draw_arc_band(cx, cy, radius, radius - stroke + 1, 0, 360, color);
            bruce_launcher__draw_thick_line(cx - radius, cy, cx + radius, cy, stroke, color);
            bruce_launcher__draw_thick_line(
                cx - radius * 4 / 5, cy - radius / 2, cx + radius * 4 / 5, cy - radius / 2, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx - radius * 4 / 5, cy + radius / 2, cx + radius * 4 / 5, cy + radius / 2, stroke, color
            );
            bruce_launcher__draw_thick_line(cx, cy - radius, cx, cy + radius, stroke, color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_BLUETOOTH:
            bruce_launcher__draw_bluetooth_rune(cx - size / 12, cy, size, stroke, color);
            bruce_launcher__draw_arc_band(cx - size / 12, cy, size * 7 / 16, size * 3 / 8, 220, 320, color);
            break;
        case BRUCE_LAUNCHER_ICON_BLUETOOTH_HID: {
            int keyboard_y = cy + size / 8;
            bruce_launcher__draw_bluetooth_rune(cx, cy - size / 6, size * 2 / 3, stroke, color);
            display__draw_round_rect(cx - size * 3 / 8, keyboard_y, size * 3 / 4, size / 3, size / 16, color);
            for (int row = 0; row < 2; ++row) {
                bruce_launcher__draw_thick_line(
                    cx - size / 4,
                    keyboard_y + size / 10 + row * size / 10,
                    cx + size / 4,
                    keyboard_y + size / 10 + row * size / 10,
                    stroke,
                    color
                );
            }
            break;
        }
        case BRUCE_LAUNCHER_ICON_IR: {
            int emitter_x = cx - size / 5;
            bruce_launcher__draw_box(
                emitter_x - size / 4, cy - size * 3 / 8, size / 8, size * 3 / 4, stroke, color
            );
            bruce_launcher__draw_box(emitter_x - size / 8, cy - size / 4, size / 8, size / 2, stroke, color);
            display__fill_circle(emitter_x, cy, size * 7 / 64, color);
            for (int radius = size * 17 / 64; radius <= size * 37 / 64; radius += size * 10 / 64) {
                bruce_launcher__draw_arc_band(emitter_x, cy, radius + stroke - 1, radius, 220, 320, color);
            }
            break;
        }
        case BRUCE_LAUNCHER_ICON_NRF24: {
            int board_w = size * 5 / 8;
            int board_h = size / 2;
            int board_x = cx - size * 3 / 8;
            int board_y = cy - board_h / 2;
            bruce_launcher__draw_box(board_x, board_y, board_w, board_h, stroke, color);
            display__draw_round_rect(
                board_x + board_w, cy - board_h / 3, size / 5, board_h * 2 / 3, size / 12, color
            );
            bruce_launcher__draw_box(cx - size / 10, cy - size / 8, size / 5, size / 4, stroke, color);
            int pin_r = size >= 48 ? 2 : 1;
            for (int row = -1; row <= 1; row += 2) {
                for (int col = 0; col < 4; ++col) {
                    display__fill_circle(
                        board_x + size / 10 + col * size / 8, cy + row * board_h / 3, pin_r, color
                    );
                }
            }
            break;
        }
        case BRUCE_LAUNCHER_ICON_FILES: {
            int page_w = size / 2;
            int page_h = size * 3 / 4;
            int step = size / 8;
            for (int page = 1; page >= -1; --page) {
                bruce_launcher__draw_box(
                    cx - page_w / 2 + page * step,
                    cy - page_h / 2 - page * step,
                    page_w,
                    page_h,
                    stroke,
                    color
                );
            }
            break;
        }
        case BRUCE_LAUNCHER_ICON_TERMINAL: {
            display__draw_round_rect(
                left + pad, top + size / 6, size - 2 * pad, size * 2 / 3, size / 12, color
            );
            int prompt_y = cy - size / 12;
            bruce_launcher__draw_thick_line(
                cx - size / 4, prompt_y - size / 10, cx - size / 8, prompt_y, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx - size / 8, prompt_y, cx - size / 4, prompt_y + size / 10, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx, prompt_y + size / 10, cx + size / 4, prompt_y + size / 10, stroke, color
            );
            break;
        }
        case BRUCE_LAUNCHER_ICON_TCP: {
            int node_r = size * 7 / 64;
            int left_x = cx - size / 3;
            int right_x = cx + size / 3;
            for (int row = -1; row <= 1; ++row) {
                int node_y = cy + row * size / 4;
                bruce_launcher__draw_thick_line(left_x, cy, right_x, node_y, stroke, color);
                display__fill_circle(right_x, node_y, node_r, color);
            }
            display__fill_circle(left_x, cy, node_r, color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_NOTIFICATION: {
            int radius = size / 3;
            int bell_y = cy + size / 8;
            bruce_launcher__draw_arc_band(cx, bell_y, radius, radius - stroke + 1, 90, 270, color);
            bruce_launcher__draw_thick_line(
                cx - radius, bell_y, cx - radius, bell_y + size / 5, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx + radius, bell_y, cx + radius, bell_y + size / 5, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx - radius - stroke,
                bell_y + size / 5,
                cx + radius + stroke,
                bell_y + size / 5,
                stroke,
                color
            );
            display__fill_circle(cx, bell_y + size / 5 + size / 12, size >= 48 ? 3 : 2, color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_APPS: {
            bruce_launcher__draw_document(cx, cy, size, stroke, color);
            int page_w = size * 40 / 64;
            int page_h = size * 60 / 64;
            int mark_y = cy + page_h / 8;
            int mark_dx = page_w / 5;
            int mark_dy = page_h / 10;
            bruce_launcher__draw_thick_line(
                cx - mark_dx / 2, mark_y, cx - mark_dx, mark_y + mark_dy, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx - mark_dx, mark_y + mark_dy, cx - mark_dx / 2, mark_y + 2 * mark_dy, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx + mark_dx / 3, mark_y + 2 * mark_dy, cx + mark_dx, mark_y, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx + mark_dx, mark_y, cx + mark_dx * 3 / 2, mark_y + mark_dy, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx + mark_dx * 3 / 2, mark_y + mark_dy, cx + mark_dx, mark_y + 2 * mark_dy, stroke, color
            );
            break;
        }
        case BRUCE_LAUNCHER_ICON_CONFIG: {
            int radius = size * 9 / 64;
            for (int tooth = 0; tooth < 6; ++tooth) {
                bruce_launcher__draw_arc_band(
                    cx, cy, radius * 7 / 2, radius * 2, 15 + 60 * tooth, 45 + 60 * tooth, color
                );
            }
            bruce_launcher__draw_arc_band(cx, cy, radius * 5 / 2, radius, 0, 360, color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_CLOCK: {
            int radius = size * 30 / 64;
            int pointer = size * 15 / 64;
            bruce_launcher__draw_arc_band(cx, cy, radius * 11 / 10, radius, 0, 360, color);
            bruce_launcher__draw_thick_line(
                cx, cy, cx - pointer * 2 / 3, cy - pointer * 2 / 3, stroke, color
            );
            bruce_launcher__draw_thick_line(cx, cy, cx + pointer, cy - pointer, stroke, color);
            display__fill_circle(cx, cy, stroke + 1, color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_SELFTEST: {
            int radius = size / 2;
            bruce_launcher__draw_arc_band(cx, cy, radius, radius - stroke + 1, 45, 315, color);
            bruce_launcher__draw_thick_line(
                cx - radius / 2, cy, cx - radius / 7, cy + radius / 3, stroke, color
            );
            bruce_launcher__draw_thick_line(
                cx - radius / 7, cy + radius / 3, cx + radius * 3 / 5, cy - radius / 3, stroke, color
            );
            break;
        }
        case BRUCE_LAUNCHER_ICON_BACK:
            bruce_launcher__draw_thick_line(left + pad, cy, left + size - pad, cy, stroke, color);
            bruce_launcher__draw_thick_line(left + pad, cy, cx, top + pad, stroke, color);
            bruce_launcher__draw_thick_line(left + pad, cy, cx, top + size - pad, stroke, color);
            break;
        case BRUCE_LAUNCHER_ICON_FOLDER: {
            int tab_h = size / 5;
            display__draw_round_rect(
                left + pad, top + tab_h, size - 2 * pad, size - tab_h - pad, size / 10, color
            );
            display__draw_round_rect(left + pad * 2, top + pad, size * 2 / 5, tab_h * 2, size / 12, color);
            break;
        }
        case BRUCE_LAUNCHER_ICON_COMMAND:
            display__draw_round_rect(left + pad, top + pad, size - 2 * pad, size - 2 * pad, size / 10, color);
            display__fill_triangle(
                cx - size / 8, cy - size / 5, cx - size / 8, cy + size / 5, cx + size / 5, cy, color
            );
            break;
    }
}
