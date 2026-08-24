#pragma once

#include <stdbool.h>

/* Root-menu rendering settings: how the main menu presents its entries.
 * Stored per-app (app_config, /config/bruce_launcher.conf) rather than in
 * bruce.conf -- this is presentation for one app, not shared device state,
 * so it follows the app_config convention instead of growing the global
 * config__t singleton. Only the root menu (bruce_launcher__run_gui_menu's
 * is_root branch) reads this; nested submenus always use Core's plain list
 * dialog regardless of layout. */

#define BRUCE_LAUNCHER_CONFIG__APP_NAME "bruce_launcher"

typedef enum {
    BRUCE_LAUNCHER_LAYOUT_CAROUSEL_H = 0, /* Original look: one big centered icon, slides left/right. */
    BRUCE_LAUNCHER_LAYOUT_CAROUSEL_V,     /* Scrolling list: icon left, label right, one row per entry. */
    BRUCE_LAUNCHER_LAYOUT_GRID,           /* Static rows/columns of icons, no slide animation. */
} bruce_launcher_layout_t;

/* Which entries show their label text in carousel layouts. Grid layouts use
 * grid_labels instead (every entry is already small and evenly spaced, so
 * "selected only" vs "all" doesn't need its own tri-state there). */
typedef enum {
    BRUCE_LAUNCHER_LABELS_SELECTED = 0, /* Only the centered/focused entry shows its label. */
    BRUCE_LAUNCHER_LABELS_ALL,          /* Every visible entry shows a (possibly truncated) label. */
    BRUCE_LAUNCHER_LABELS_NONE,         /* Icons only, no label text anywhere. */
} bruce_launcher_label_mode_t;

typedef struct {
    bruce_launcher_layout_t layout;
    bruce_launcher_label_mode_t carousel_labels;
    int grid_columns;  /* 0 = auto (picked from display width at render time). */
    bool grid_labels;  /* Show a label under every grid icon vs. hide labels entirely. */
} bruce_launcher_render_config_t;

/* Reads the current render settings from app_config, filling in defaults
 * (BRUCE_LAUNCHER_LAYOUT_CAROUSEL_H / LABELS_SELECTED / auto columns / labels
 * on) for anything unset or unrecognized. Never fails. */
void bruce_launcher_config__load(bruce_launcher_render_config_t *out);

/* Entry point for "bruce_launcher config ...": a GUI screen (dialog__choice_launcher
 * loop, mirroring config_app's display/theme screens) when runtime__gui_requested(),
 * an ArgParser CLI otherwise. `argc`/`argv` are everything after "config",
 * with argv[0] standing in for the sub-command's own program name (ap_parse's
 * usual convention) -- i.e. bruce_launcher_app_main should call this with
 * (argc - 1, argv + 1) relative to its own arguments. */
int bruce_launcher_config__run(int argc, char **argv);
