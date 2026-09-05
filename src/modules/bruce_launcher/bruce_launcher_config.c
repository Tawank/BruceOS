#include "bruce_launcher_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "bruce_launcher_menu_editor.h"
#include "core_sdk/app_config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

/* Highest grid_columns value the settings screen/CLI accept. Not a hard
 * rendering limit -- bruce_launcher_app.c's grid renderer clamps further
 * against entry_count and display width -- just a sane upper bound so a
 * typo'd CLI value ("grid-columns 400") doesn't get stored verbatim. */
#define BRUCE_LAUNCHER_CONFIG__GRID_COLUMNS_MAX 8

static const char *const LAYOUT_NAMES[3] = {"carousel_h", "carousel_v", "grid"};
static const char *const LAYOUT_LABELS[3] = {"Horizontal Carousel", "Vertical List", "Grid"};
static const char *const LABEL_MODE_NAMES[3] = {"selected", "all", "none"};
static const char *const LABEL_MODE_LABELS[3] = {"Main item only", "All items", "Hidden"};

static bruce_launcher_layout_t bruce_launcher_config__parse_layout(const char *text) {
    if (strcmp(text, LAYOUT_NAMES[BRUCE_LAUNCHER_LAYOUT_CAROUSEL_V]) == 0) {
        return BRUCE_LAUNCHER_LAYOUT_CAROUSEL_V;
    }
    if (strcmp(text, LAYOUT_NAMES[BRUCE_LAUNCHER_LAYOUT_GRID]) == 0) return BRUCE_LAUNCHER_LAYOUT_GRID;
    return BRUCE_LAUNCHER_LAYOUT_CAROUSEL_H;
}

static bruce_launcher_label_mode_t bruce_launcher_config__parse_labels(const char *text) {
    if (strcmp(text, LABEL_MODE_NAMES[BRUCE_LAUNCHER_LABELS_ALL]) == 0) return BRUCE_LAUNCHER_LABELS_ALL;
    if (strcmp(text, LABEL_MODE_NAMES[BRUCE_LAUNCHER_LABELS_NONE]) == 0) return BRUCE_LAUNCHER_LABELS_NONE;
    return BRUCE_LAUNCHER_LABELS_SELECTED;
}

void bruce_launcher_config__load(bruce_launcher_render_config_t *out) {
    char text[BRUCE_APP_CONFIG_STRING_MAX_LEN];

    app_config__get_string(
        BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.layout", LAYOUT_NAMES[BRUCE_LAUNCHER_LAYOUT_CAROUSEL_H],
        text, sizeof(text)
    );
    out->layout = bruce_launcher_config__parse_layout(text);

    app_config__get_string(
        BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.carousel_labels",
        LABEL_MODE_NAMES[BRUCE_LAUNCHER_LABELS_SELECTED], text, sizeof(text)
    );
    out->carousel_labels = bruce_launcher_config__parse_labels(text);

    int columns = app_config__get_int(BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.grid_columns", 0);
    out->grid_columns = (columns >= 0 && columns <= BRUCE_LAUNCHER_CONFIG__GRID_COLUMNS_MAX) ? columns : 0;

    out->grid_labels = app_config__get_bool(BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.grid_labels", true);
}

static bruce_result_t bruce_launcher_config__set_layout(bruce_launcher_layout_t layout) {
    return app_config__set_string(BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.layout", LAYOUT_NAMES[layout]);
}

static bruce_result_t bruce_launcher_config__set_carousel_labels(bruce_launcher_label_mode_t mode) {
    return app_config__set_string(
        BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.carousel_labels", LABEL_MODE_NAMES[mode]
    );
}

static bruce_result_t bruce_launcher_config__set_grid_columns(int columns) {
    return app_config__set_int(BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.grid_columns", columns);
}

static bruce_result_t bruce_launcher_config__set_grid_labels(bool show) {
    return app_config__set_bool(BRUCE_LAUNCHER_CONFIG__APP_NAME, "render.grid_labels", show);
}

/* -------------------------------------------------------------------------- */
/* GUI screen                                                                 */
/* -------------------------------------------------------------------------- */

/* Each picker mirrors config_app.c's sub-dialog pattern: cancelling it just
 * redraws the settings screen unchanged (BRUCE_OK), it never bubbles
 * BRUCE_ERR_CANCELLED out of the whole screen. Only a real storage failure
 * from the app_config setter is treated as an error worth returning. */

static bruce_result_t bruce_launcher_config__pick_layout(bruce_launcher_layout_t current) {
    bruce_dialog_choice_t choices[3];
    for (size_t i = 0; i < 3; ++i) {
        choices[i] = (bruce_dialog_choice_t){.label = LAYOUT_LABELS[i], .value = LAYOUT_NAMES[i]};
    }
    size_t selected = (size_t)current;
    bruce_result_t result = dialog__choice_launcher("Layout", NULL, choices, 3, &selected);
    if (result != BRUCE_OK) return BRUCE_OK;
    return bruce_launcher_config__set_layout((bruce_launcher_layout_t)selected);
}

static bruce_result_t bruce_launcher_config__pick_carousel_labels(bruce_launcher_label_mode_t current) {
    bruce_dialog_choice_t choices[3];
    for (size_t i = 0; i < 3; ++i) {
        choices[i] = (bruce_dialog_choice_t){.label = LABEL_MODE_LABELS[i], .value = LABEL_MODE_NAMES[i]};
    }
    size_t selected = (size_t)current;
    bruce_result_t result = dialog__choice_launcher("Carousel Labels", NULL, choices, 3, &selected);
    if (result != BRUCE_OK) return BRUCE_OK;
    return bruce_launcher_config__set_carousel_labels((bruce_launcher_label_mode_t)selected);
}

static bruce_result_t bruce_launcher_config__pick_grid_columns(int current) {
    static const int values[6] = {0, 2, 3, 4, 5, 6};
    static const char *const labels[6] = {"Auto", "2", "3", "4", "5", "6"};
    bruce_dialog_choice_t choices[6];
    size_t selected = 0;
    for (size_t i = 0; i < 6; ++i) {
        choices[i] = (bruce_dialog_choice_t){.label = labels[i], .value = labels[i]};
        if (values[i] == current) selected = i;
    }
    bruce_result_t result = dialog__choice_launcher("Grid Columns", NULL, choices, 6, &selected);
    if (result != BRUCE_OK) return BRUCE_OK;
    return bruce_launcher_config__set_grid_columns(values[selected]);
}

static bruce_result_t bruce_launcher_config__pick_grid_labels(bool current) {
    bruce_dialog_choice_t choices[2] = {
        {.label = "On", .value = "on"},
        {.label = "Off", .value = "off"},
    };
    size_t selected = current ? 0 : 1;
    bruce_result_t result = dialog__choice_launcher("Grid Labels", NULL, choices, 2, &selected);
    if (result != BRUCE_OK) return BRUCE_OK;
    return bruce_launcher_config__set_grid_labels(selected == 0);
}

static int bruce_launcher_config__gui(void) {
    for (;;) {
        bruce_launcher_render_config_t cfg;
        bruce_launcher_config__load(&cfg);

        char layout_label[40];
        char labels_label[40];
        char columns_label[24];
        char grid_labels_label[24];
        snprintf(layout_label, sizeof(layout_label), "Layout: %s", LAYOUT_LABELS[cfg.layout]);
        snprintf(
            labels_label, sizeof(labels_label), "Carousel labels: %s", LABEL_MODE_LABELS[cfg.carousel_labels]
        );
        if (cfg.grid_columns == 0) snprintf(columns_label, sizeof(columns_label), "Grid columns: Auto");
        else snprintf(columns_label, sizeof(columns_label), "Grid columns: %d", cfg.grid_columns);
        snprintf(
            grid_labels_label, sizeof(grid_labels_label), "Grid labels: %s", cfg.grid_labels ? "On" : "Off"
        );

        bruce_dialog_choice_t choices[5];
        size_t count = 0;
        choices[count++] = (bruce_dialog_choice_t){.label = layout_label, .value = "layout"};
        if (cfg.layout == BRUCE_LAUNCHER_LAYOUT_GRID) {
            choices[count++] = (bruce_dialog_choice_t){.label = columns_label, .value = "grid_columns"};
            choices[count++] = (bruce_dialog_choice_t){.label = grid_labels_label, .value = "grid_labels"};
        } else {
            choices[count++] = (bruce_dialog_choice_t){.label = labels_label, .value = "carousel_labels"};
        }
        choices[count++] = (bruce_dialog_choice_t){.label = "Menu entries", .value = "menu_entries"};
        choices[count++] = (bruce_dialog_choice_t){.label = "Back", .value = "back"};

        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher("Launcher Layout", NULL, choices, count, &selected);
        bool back = result == BRUCE_ERR_CANCELLED ||
                    (result == BRUCE_OK && strcmp(choices[selected].value, "back") == 0);
        if (back) return BRUCE_OK;
        if (result != BRUCE_OK) continue;

        const char *action = choices[selected].value;
        bruce_result_t set_result = BRUCE_OK;
        if (strcmp(action, "layout") == 0) set_result = bruce_launcher_config__pick_layout(cfg.layout);
        else if (strcmp(action, "carousel_labels") == 0)
            set_result = bruce_launcher_config__pick_carousel_labels(cfg.carousel_labels);
        else if (strcmp(action, "grid_columns") == 0)
            set_result = bruce_launcher_config__pick_grid_columns(cfg.grid_columns);
        else if (strcmp(action, "grid_labels") == 0)
            set_result = bruce_launcher_config__pick_grid_labels(cfg.grid_labels);
        else if (strcmp(action, "menu_entries") == 0) set_result = bruce_launcher_menu_editor__run();
        if (set_result != BRUCE_OK) return set_result;
    }
}

/* -------------------------------------------------------------------------- */
/* CLI                                                                        */
/* -------------------------------------------------------------------------- */

static bool bruce_launcher_config__parse_on_off(const char *text, bool *out) {
    if (strcasecmp(text, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(text, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static int bruce_launcher_config__cli_show(void) {
    bruce_launcher_render_config_t cfg;
    bruce_launcher_config__load(&cfg);
    char columns_text[16];
    if (cfg.grid_columns == 0) snprintf(columns_text, sizeof(columns_text), "auto");
    else snprintf(columns_text, sizeof(columns_text), "%d", cfg.grid_columns);
    stdio__printf(
        "Layout: %s\n"
        "Carousel labels: %s\n"
        "Grid columns: %s\n"
        "Grid labels: %s\n",
        LAYOUT_NAMES[cfg.layout], LABEL_MODE_NAMES[cfg.carousel_labels], columns_text,
        cfg.grid_labels ? "on" : "off"
    );
    return BRUCE_OK;
}

static int bruce_launcher_config__layout_cli(const char *mode) {
    if (mode == NULL) {
        bruce_launcher_render_config_t cfg;
        bruce_launcher_config__load(&cfg);
        stdio__printf("Layout: %s\n", LAYOUT_NAMES[cfg.layout]);
        return BRUCE_OK;
    }
    if (strcasecmp(mode, "carousel-h") == 0 || strcasecmp(mode, "carousel_h") == 0) {
        return bruce_launcher_config__set_layout(BRUCE_LAUNCHER_LAYOUT_CAROUSEL_H);
    }
    if (strcasecmp(mode, "carousel-v") == 0 || strcasecmp(mode, "carousel_v") == 0) {
        return bruce_launcher_config__set_layout(BRUCE_LAUNCHER_LAYOUT_CAROUSEL_V);
    }
    if (strcasecmp(mode, "grid") == 0) return bruce_launcher_config__set_layout(BRUCE_LAUNCHER_LAYOUT_GRID);
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static int bruce_launcher_config__carousel_labels_cli(const char *mode) {
    if (mode == NULL) {
        bruce_launcher_render_config_t cfg;
        bruce_launcher_config__load(&cfg);
        stdio__printf("Carousel labels: %s\n", LABEL_MODE_NAMES[cfg.carousel_labels]);
        return BRUCE_OK;
    }
    if (strcasecmp(mode, "selected") == 0) {
        return bruce_launcher_config__set_carousel_labels(BRUCE_LAUNCHER_LABELS_SELECTED);
    }
    if (strcasecmp(mode, "all") == 0) {
        return bruce_launcher_config__set_carousel_labels(BRUCE_LAUNCHER_LABELS_ALL);
    }
    if (strcasecmp(mode, "none") == 0) {
        return bruce_launcher_config__set_carousel_labels(BRUCE_LAUNCHER_LABELS_NONE);
    }
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static int bruce_launcher_config__grid_columns_cli(const char *columns) {
    if (columns == NULL) {
        bruce_launcher_render_config_t cfg;
        bruce_launcher_config__load(&cfg);
        if (cfg.grid_columns == 0) stdio__printf("Grid columns: auto\n");
        else stdio__printf("Grid columns: %d\n", cfg.grid_columns);
        return BRUCE_OK;
    }
    if (strcasecmp(columns, "auto") == 0) return bruce_launcher_config__set_grid_columns(0);
    char *end = NULL;
    long value = strtol(columns, &end, 10);
    if (end == columns || *end != '\0' || value < 2 || value > BRUCE_LAUNCHER_CONFIG__GRID_COLUMNS_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return bruce_launcher_config__set_grid_columns((int)value);
}

static int bruce_launcher_config__grid_labels_cli(const char *state) {
    if (state == NULL) {
        bruce_launcher_render_config_t cfg;
        bruce_launcher_config__load(&cfg);
        stdio__printf("Grid labels: %s\n", cfg.grid_labels ? "on" : "off");
        return BRUCE_OK;
    }
    bool value;
    return bruce_launcher_config__parse_on_off(state, &value) ? bruce_launcher_config__set_grid_labels(value)
                                                                : BRUCE_ERR_INVALID_ARGUMENT;
}

int bruce_launcher_config__run(int argc, char **argv) {
    if (runtime__gui_requested()) return bruce_launcher_config__gui();

    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Configure the main-menu rendering style.");

    ArgParser *layout = ap_new_cmd(root, "layout");
    ArgParser *carousel_labels = ap_new_cmd(root, "carousel-labels");
    ArgParser *grid_columns = ap_new_cmd(root, "grid-columns");
    ArgParser *grid_labels = ap_new_cmd(root, "grid-labels");
    if (layout == NULL || carousel_labels == NULL || grid_columns == NULL || grid_labels == NULL) {
        ap_free(root);
        return BRUCE_ERR_NO_MEMORY;
    }

    ap_set_helptext(layout, "Select the root-menu layout.");
    ap_add_optional_arg(layout, "mode", "carousel-h, carousel-v, or grid (required outside GUI mode)");
    ap_set_helptext(carousel_labels, "Select which carousel entries show their label.");
    ap_add_optional_arg(carousel_labels, "mode", "selected, all, or none (required outside GUI mode)");
    ap_set_helptext(grid_columns, "Set the grid layout's column count.");
    ap_add_optional_arg(grid_columns, "columns", "auto, or 2-8 (required outside GUI mode)");
    ap_set_helptext(grid_labels, "Show or hide labels under grid icons.");
    ap_add_optional_arg(grid_labels, "state", "on or off (required outside GUI mode)");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    ArgParser *action = ap_get_cmd_parser(root);
    int result;
    if (action == NULL) result = bruce_launcher_config__cli_show();
    else if (action == layout) result = bruce_launcher_config__layout_cli(ap_get_arg(layout, "mode"));
    else if (action == carousel_labels) {
        result = bruce_launcher_config__carousel_labels_cli(ap_get_arg(carousel_labels, "mode"));
    } else if (action == grid_columns) {
        result = bruce_launcher_config__grid_columns_cli(ap_get_arg(grid_columns, "columns"));
    } else if (action == grid_labels) {
        result = bruce_launcher_config__grid_labels_cli(ap_get_arg(grid_labels, "state"));
    } else {
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
