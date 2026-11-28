#include "rf_app.h"

#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"
#include "core_sdk/rf.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define RF_APP_TITLE_H 12
#define RF_APP_FOOTER_H 12
#define RF_APP_ROW_H 10
#define RF_APP_CHAR_W 6
#define RF_APP_MIN_DBM (-110)
#define RF_APP_MAX_DBM (-30)
#define RF_APP_SIGNAL_DBM (-80) /* Weaker than this is noise. */
#define RF_APP_PEAK_DECAY 2     /* dB a peak falls each sweep. */
#define RF_APP_HIDDEN_DELAY_MS 100
#define RF_APP_INPUT_MS 20 /* Between sweeps. */

/* Where sub-GHz remotes, sensors and doorbells usually send. */
static const uint32_t RF_APP_FREQUENCIES_KHZ[] = {
    300000, 303875, 304250, 310000, 315000, 318000, 390000, 418000,
    433075, 433920, 434420, 434775, 438900, 868350, 915000,
};
#define RF_APP_FREQUENCY_COUNT (sizeof(RF_APP_FREQUENCIES_KHZ) / sizeof(RF_APP_FREQUENCIES_KHZ[0]))

typedef struct {
    size_t module;
    bruce_rf_module_t info;
    uint32_t khz[RF_APP_FREQUENCY_COUNT];
    int16_t dbm[RF_APP_FREQUENCY_COUNT];
    int16_t peak[RF_APP_FREQUENCY_COUNT];
    size_t count;
    size_t first_row; /* Scrolled down to, when the rows don't fit. */
} rf_app_scan_t;

static bool rf_app__tunes(const bruce_rf_module_t *info, uint32_t khz) {
    for (uint8_t i = 0; i < info->band_count; ++i) {
        if (khz >= info->bands[i].min_khz && khz <= info->bands[i].max_khz) return true;
    }
    return false;
}

/* By name, or the first one no other app holds. */
static bruce_result_t rf_app__find(const char *name, size_t *out_index, bruce_rf_module_t *out_info) {
    size_t count = rf__count();
    for (size_t i = 0; i < count; ++i) {
        if (rf__info(i, out_info) != BRUCE_OK) continue;
        if (name != NULL ? strcmp(out_info->name, name) == 0 : !out_info->in_use) {
            *out_index = i;
            return BRUCE_OK;
        }
    }
    return count == 0 || name != NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_BUSY;
}

static bruce_result_t rf_app__sweep(rf_app_scan_t *scan) {
    for (size_t i = 0; i < scan->count; ++i) {
        bruce_result_t result = rf__rssi(scan->module, scan->khz[i], &scan->dbm[i]);
        if (result != BRUCE_OK) return result;
        int16_t decayed = (int16_t)(scan->peak[i] - RF_APP_PEAK_DECAY);
        scan->peak[i] = scan->dbm[i] > decayed ? scan->dbm[i] : decayed;
    }
    return BRUCE_OK;
}

static int rf_app__bar_width(int16_t dbm, int width) {
    if (dbm <= RF_APP_MIN_DBM) return 0;
    if (dbm >= RF_APP_MAX_DBM) return width;
    return (dbm - RF_APP_MIN_DBM) * width / (RF_APP_MAX_DBM - RF_APP_MIN_DBM);
}

static void rf_app__draw(const rf_app_scan_t *scan) {
    uint16_t background = config__get_color_background();
    uint16_t primary = config__get_color_primary();
    uint16_t text = config__get_color_text();
    int width = display__width();
    int height = display__height();
    if (display__begin_frame() != BRUCE_OK) return;
    (void)display__fill_screen(background);
    (void)display__set_text_size(1);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__fill_rect(0, 0, (int16_t)width, RF_APP_TITLE_H, primary);
    (void)display__set_text_color(background);
    (void)display__set_cursor(2, 2);
    char line[48];
    snprintf(line, sizeof(line), "RF scan  %s", scan->info.chip);
    (void)display__print(line);

    size_t strongest = scan->count;
    for (size_t i = 0; i < scan->count; ++i) {
        if (scan->dbm[i] >= RF_APP_SIGNAL_DBM && (strongest == scan->count || scan->dbm[i] > scan->dbm[strongest])) {
            strongest = i;
        }
    }

    int label_w = 8 * RF_APP_CHAR_W; /* "433.920 " */
    int value_w = 5 * RF_APP_CHAR_W; /* " -110" */
    int bar_x = 2 + label_w;
    int bar_w = width - bar_x - value_w - 2;
    int rows = (height - RF_APP_TITLE_H - RF_APP_FOOTER_H) / RF_APP_ROW_H;
    for (int row = 0; row < rows && scan->first_row + (size_t)row < scan->count; ++row) {
        size_t i = scan->first_row + (size_t)row;
        int y = RF_APP_TITLE_H + 2 + row * RF_APP_ROW_H;
        (void)display__set_text_color(text);
        (void)display__set_cursor(2, (int16_t)(y + 1));
        snprintf(line, sizeof(line), "%3u.%03u", (unsigned)(scan->khz[i] / 1000u), (unsigned)(scan->khz[i] % 1000u));
        (void)display__print(line);
        (void)display__draw_rect((int16_t)bar_x, (int16_t)y, (int16_t)bar_w, RF_APP_ROW_H - 2, primary);
        int filled = rf_app__bar_width(scan->dbm[i], bar_w);
        if (filled > 0) (void)display__fill_rect((int16_t)bar_x, (int16_t)y, (int16_t)filled, RF_APP_ROW_H - 2, primary);
        int peak = rf_app__bar_width(scan->peak[i], bar_w);
        if (peak > 0) (void)display__fill_rect((int16_t)(bar_x + peak - 1), (int16_t)y, 2, RF_APP_ROW_H - 2, text);
        snprintf(line, sizeof(line), "%5d", scan->dbm[i]);
        (void)display__set_cursor((int16_t)(width - value_w - 2), (int16_t)(y + 1));
        (void)display__print(line);
    }

    (void)display__set_text_color(text);
    (void)display__set_cursor(2, (int16_t)(height - RF_APP_FOOTER_H + 2));
    if (strongest < scan->count) {
        snprintf(line, sizeof(line), "Signal %u.%03u MHz %d dBm", (unsigned)(scan->khz[strongest] / 1000u),
                 (unsigned)(scan->khz[strongest] % 1000u), scan->dbm[strongest]);
    } else {
        snprintf(line, sizeof(line), "No signal   Back: exit");
    }
    (void)display__print(line);
    (void)display__present();
}

static bool rf_app__is_back(const bruce_input_event_t *event) {
    bool key = event->type == BRUCE_INPUT_KEY && event->value != event->code;
    return (event->code == BRUCE_INPUT_CODE_BACK && (key || event->type != BRUCE_INPUT_KEY)) ||
           event->code == BRUCE_INPUT_CODE_BUTTON_B;
}

/* Sweeps until Back, drawing after each sweep. */
static bruce_result_t rf_app__scan_gui(rf_app_scan_t *scan) {
    int rows = (display__height() - RF_APP_TITLE_H - RF_APP_FOOTER_H) / RF_APP_ROW_H;
    size_t last_first = scan->count > (size_t)rows ? scan->count - (size_t)rows : 0;
    (void)input__flush();
    for (;;) {
        bruce_input_event_t event;
        bruce_result_t input = input__read(&event, RF_APP_INPUT_MS);
        if (input == BRUCE_ERR_NOT_FOREGROUND) {
            (void)runtime__delay(RF_APP_HIDDEN_DELAY_MS);
            continue;
        }
        if (input == BRUCE_OK && event.action == BRUCE_INPUT_PRESS) {
            if (rf_app__is_back(&event)) return BRUCE_OK;
            bool key = event.type != BRUCE_INPUT_KEY || event.value != event.code;
            if (key && (event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_PREV) && scan->first_row > 0) {
                scan->first_row--;
            } else if (key && (event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT) &&
                       scan->first_row < last_first) {
                scan->first_row++;
            }
        }
        bruce_result_t result = rf_app__sweep(scan);
        if (result != BRUCE_OK) return result;
        rf_app__draw(scan);
    }
}

static bruce_result_t rf_app__scan(const char *name, bool gui) {
    static rf_app_scan_t scan; /* Off the stack. */
    memset(&scan, 0, sizeof(scan));
    bruce_result_t result = rf_app__find(name, &scan.module, &scan.info);
    if (result == BRUCE_OK) result = rf__open(scan.module);
    if (result != BRUCE_OK) {
        const char *message = result == BRUCE_ERR_NOT_FOUND ? "No RF module found"
                              : result == BRUCE_ERR_BUSY    ? "The RF module is in use by another app"
                                                            : "Could not start the RF module";
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "RF scan", message);
        else stdio__printf("rf: %s (%s)\n", message, result__to_string(result));
        return result;
    }
    for (size_t i = 0; i < RF_APP_FREQUENCY_COUNT; ++i) {
        if (!rf_app__tunes(&scan.info, RF_APP_FREQUENCIES_KHZ[i])) continue;
        scan.khz[scan.count] = RF_APP_FREQUENCIES_KHZ[i];
        scan.peak[scan.count] = RF_APP_MIN_DBM;
        scan.count++;
    }
    if (gui) {
        result = rf_app__scan_gui(&scan);
    } else {
        result = rf_app__sweep(&scan);
        for (size_t i = 0; result == BRUCE_OK && i < scan.count; ++i) {
            stdio__printf("%3u.%03u MHz  %4d dBm\n", (unsigned)(scan.khz[i] / 1000u),
                          (unsigned)(scan.khz[i] % 1000u), scan.dbm[i]);
        }
    }
    (void)rf__close(scan.module);
    if (result != BRUCE_OK) {
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "RF scan", "The RF module stopped answering");
        else stdio__printf("rf: scan failed (%s)\n", result__to_string(result));
    }
    return result;
}

static void rf_app__list(void) {
    size_t count = rf__count();
    if (count == 0) stdio__printf("No RF modules\n");
    for (size_t i = 0; i < count; ++i) {
        bruce_rf_module_t info;
        if (rf__info(i, &info) != BRUCE_OK) continue;
        stdio__printf("%s: %s%s,", info.name, info.chip, info.in_use ? " (in use)" : "");
        for (uint8_t b = 0; b < info.band_count; ++b) {
            stdio__printf(" %u-%u MHz", (unsigned)(info.bands[b].min_khz / 1000u),
                          (unsigned)(info.bands[b].max_khz / 1000u));
        }
        stdio__printf("\n");
    }
}

static int rf_app__gui(void) {
    static const bruce_dialog_choice_t choices[] = {
        {.label = "Scan", .value = "scan"},
        {.label = "Exit", .value = "exit"},
    };
    for (;;) {
        size_t selected = 0;
        bruce_result_t result =
            dialog__choice_launcher("RF", NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected);
        if (result == BRUCE_ERR_CANCELLED || (result == BRUCE_OK && selected == 1)) return 0;
        if (result != BRUCE_OK) return result;
        (void)rf_app__scan(NULL, true);
    }
}

/* The launcher's RF submenu runs "rf scan" (embedded_resources/json/launcher.json). */
int rf_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Use sub-GHz radio modules (SX1262, CC1101).");
    ArgParser *scan = ap_new_cmd(root, "scan");
    ArgParser *list = ap_new_cmd(root, "list");
    ap_set_helptext(scan, "Measure the signal on common sub-GHz frequencies.");
    ap_add_optional_arg(scan, "module", "Module name (default: the first free one)");
    ap_set_helptext(list, "List the RF modules.");
    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION) {
            ap_print_help(ap_get_cmd_parser(root) != NULL ? ap_get_cmd_parser(root) : root);
        }
        ap_free(root);
        return status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
               : status == AP_STATUS_NO_MEMORY                       ? BRUCE_ERR_NO_MEMORY
                                                                     : BRUCE_ERR_INVALID_ARGUMENT;
    }
    ArgParser *command = ap_get_cmd_parser(root);
    bool gui = runtime__is_gui_requested();
    int result = BRUCE_OK;
    if (command == scan) {
        char name[BRUCE_RF_NAME_MAX] = {0};
        const char *module = ap_get_arg(scan, "module");
        if (module != NULL) snprintf(name, sizeof(name), "%s", module);
        ap_free(root);
        return rf_app__scan(module != NULL ? name : NULL, gui);
    }
    if (command == list || !gui) rf_app__list();
    else result = rf_app__gui();
    ap_free(root);
    return result;
}
