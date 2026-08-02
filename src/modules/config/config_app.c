#include "config_app.h"

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int config_app__show_clock(void) {
    bool automatic = config__get_automatic_time_update_via_ntp();
    bool dst = config__get_dst();
    bool format24 = config__get_clock24hr();
    float timezone = config__get_tmz();
    bruce_clock_datetime_t now;
    bruce_result_t clock_result = clock__get_local(&now);
    if (clock_result == BRUCE_OK) {
        stdio__printf("Local time: %04u-%02u-%02u %02u:%02u:%02u\n", now.year, now.month, now.day, now.hour,
                      now.minute, now.second);
    } else stdio__printf("Local time: not set\n");
    stdio__printf("NTP: %s (%s)\n", automatic ? "automatic" : "manual", clock__get_ntp_server());
    stdio__printf("Timezone: UTC%+.2f\nDST: %s\nFormat: %s\n", timezone, dst ? "on" : "off",
                  format24 ? "24-hour" : "12-hour");
    return BRUCE_OK;
}

static bool config_app__parse_datetime(const char *date, const char *time, bruce_clock_datetime_t *out) {
    unsigned int year, month, day, hour, minute, second;
    char extra;
    if (date == NULL || time == NULL || sscanf(date, "%u-%u-%u%c", &year, &month, &day, &extra) != 3 ||
        sscanf(time, "%u:%u:%u%c", &hour, &minute, &second, &extra) != 3) return false;
    *out = (bruce_clock_datetime_t){year, month, day, hour, minute, second};
    return true;
}

static bruce_result_t config_app__manual_dialog(void) {
    bruce_clock_datetime_t now = {2026, 1, 1, 0, 0, 0};
    (void)clock__get_local(&now);
    char initial_date[16], initial_time[16], date[16], time[16];
    snprintf(initial_date, sizeof(initial_date), "%04u-%02u-%02u", now.year, now.month, now.day);
    snprintf(initial_time, sizeof(initial_time), "%02u:%02u:%02u", now.hour, now.minute, now.second);
    if (dialog__text_input("Manual clock", "Date YYYY-MM-DD", initial_date, false, date, sizeof(date)) != BRUCE_OK ||
        dialog__text_input("Manual clock", "Time HH:MM:SS", initial_time, false, time, sizeof(time)) != BRUCE_OK) {
        return BRUCE_ERR_CANCELLED;
    }
    bruce_clock_datetime_t value;
    if (!config_app__parse_datetime(date, time, &value)) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = clock__set_local(&value);
    if (result == BRUCE_OK) (void)config__set_automatic_time_update_via_ntp(false);
    return result;
}

static int config_app__clock_gui(void) {
    for (;;) {
        bool automatic = config__get_automatic_time_update_via_ntp();
        bool dst = config__get_dst();
        bool format24 = config__get_clock24hr();
        float timezone = config__get_tmz();
        char ntp_label[40], timezone_label[40], dst_label[32], format_label[32];
        snprintf(ntp_label, sizeof(ntp_label), "Automatic NTP: %s", automatic ? "ON" : "OFF");
        snprintf(timezone_label, sizeof(timezone_label), "Timezone: UTC%+.2f", timezone);
        snprintf(dst_label, sizeof(dst_label), "Daylight savings: %s", dst ? "ON" : "OFF");
        snprintf(format_label, sizeof(format_label), "Clock format: %s", format24 ? "24-hour" : "12-hour");
        const bruce_dialog_choice_t choices[] = {
            {.label = "Sync from NTP now", .value = "sync"},
            {.label = ntp_label, .value = "automatic"},
            {.label = timezone_label, .value = "timezone"},
            {.label = dst_label, .value = "dst"},
            {.label = format_label, .value = "format"},
            {.label = "Set date and time manually", .value = "manual"},
            {.label = "Back", .value = "back"},
        };
        size_t selected = 0;
        bruce_result_t result = dialog__choice("System clock", "UTC system time, local display", choices, 7,
                                               &selected, NULL);
        if (result == BRUCE_ERR_CANCELLED || selected == 6) return BRUCE_OK;
        if (result != BRUCE_OK) return result;
        if (selected == 0) {
            result = clock__sync_ntp(10000);
            (void)dialog__message(result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_ERROR, "NTP sync",
                                  result == BRUCE_OK ? "Clock synchronized" : "Connect Wi-Fi and try again");
        } else if (selected == 1) {
            (void)config__set_automatic_time_update_via_ntp(!automatic);
        } else if (selected == 2) {
            char initial[16], entered[16];
            snprintf(initial, sizeof(initial), "%.2f", timezone);
            if (dialog__number_input("Timezone", "UTC offset (-12 to +14)", initial, entered, sizeof(entered)) ==
                BRUCE_OK) {
                char *end = NULL;
                float value = strtof(entered, &end);
                if (end != entered && *end == '\0' && value >= -12.0f && value <= 14.0f)
                    (void)config__set_tmz(value);
                else (void)dialog__message(BRUCE_DIALOG_ERROR, "Timezone", "Offset must be from -12 to +14");
            }
        } else if (selected == 3) {
            (void)config__set_dst(!dst);
        } else if (selected == 4) {
            (void)config__set_clock24hr(!format24);
        } else {
            result = config_app__manual_dialog();
            if (result != BRUCE_OK && result != BRUCE_ERR_CANCELLED)
                (void)dialog__message(BRUCE_DIALOG_ERROR, "Manual clock", "Invalid date or time");
        }
    }
}

static bool config_app__parse_on_off(const char *text, bool *out) {
    if (text == NULL) return false;
    if (strcmp(text, "on") == 0) *out = true;
    else if (strcmp(text, "off") == 0) *out = false;
    else return false;
    return true;
}

static int config_app__clock_cli(ArgParser *clock_parser, ArgParser *show, ArgParser *sync, ArgParser *ntp,
                                 ArgParser *timezone, ArgParser *dst, ArgParser *format, ArgParser *set) {
    ArgParser *action = ap_get_cmd_parser(clock_parser);
    if (action == NULL || action == show) return config_app__show_clock();
    if (action == sync) {
        bruce_result_t result = clock__sync_ntp(10000);
        stdio__printf(result == BRUCE_OK ? "Clock synchronized\n" : "NTP synchronization failed: %d\n", result);
        return result;
    }
    if (action == ntp) {
        bool value;
        return config_app__parse_on_off(ap_get_arg(ntp, "state"), &value)
                   ? config__set_automatic_time_update_via_ntp(value)
                   : BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (action == timezone) {
        const char *offset = ap_get_arg(timezone, "offset");
        if (offset == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
        char *end = NULL;
        float value = strtof(offset, &end);
        if (end == offset || *end != '\0' || value < -12.0f || value > 14.0f) return BRUCE_ERR_INVALID_ARGUMENT;
        return config__set_tmz(value);
    }
    if (action == dst) {
        bool value;
        return config_app__parse_on_off(ap_get_arg(dst, "state"), &value) ? config__set_dst(value)
                                                                          : BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (action == format) {
        const char *value = ap_get_arg(format, "hours");
        if (value == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
        if (strcmp(value, "12") == 0) return config__set_clock24hr(false);
        if (strcmp(value, "24") == 0) return config__set_clock24hr(true);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (action == set) {
        bruce_clock_datetime_t value;
        if (!config_app__parse_datetime(ap_get_arg(set, "date"), ap_get_arg(set, "time"), &value))
            return BRUCE_ERR_INVALID_ARGUMENT;
        bruce_result_t result = clock__set_local(&value);
        if (result == BRUCE_OK) (void)config__set_automatic_time_update_via_ntp(false);
        return result;
    }
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static int config_app__startup_cli(ArgParser *startup_parser, ArgParser *add, ArgParser *remove) {
    ArgParser *action = ap_get_cmd_parser(startup_parser);
    if (action == add) return config__add_startup_app(ap_get_arg(add, "name"));
    if (action == remove) return config__remove_startup_app(ap_get_arg(remove, "name"));
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static int config_app__display_gui(void) {
    for (;;) {
        bool buffered = config__get_display_buffered_rendering();
        char buffered_label[48];
        snprintf(buffered_label, sizeof(buffered_label), "Buffered rendering: %s", buffered ? "ON" : "OFF");
        const bruce_dialog_choice_t choices[] = {
            {.label = buffered_label, .value = "buffered"},
            {.label = "Back", .value = "back"},
        };
        size_t selected = 0;
        bruce_result_t result = dialog__choice(
            "Display rendering",
            buffered ? "Smooth complete frames; uses about 65 KB RAM"
                     : "Direct drawing saves RAM; screenshots unavailable",
            choices,
            2,
            &selected,
            NULL
        );
        if (result == BRUCE_ERR_CANCELLED || selected == 1) return BRUCE_OK;
        if (result != BRUCE_OK) return result;
        result = config__set_display_buffered_rendering(!buffered);
        if (result != BRUCE_OK) return result;
        (void)dialog__message(BRUCE_DIALOG_INFO, "Display rendering", "Setting saved; reboot to apply");
    }
}

static int config_app__display_cli(ArgParser *display_parser, ArgParser *buffered) {
    ArgParser *action = ap_get_cmd_parser(display_parser);
    if (action == NULL) {
        stdio__printf(
            "Buffered rendering: %s (reboot required after changes)\n",
            config__get_display_buffered_rendering() ? "on" : "off"
        );
        return BRUCE_OK;
    }
    if (action != buffered) return BRUCE_ERR_INVALID_ARGUMENT;
    const char *state = ap_get_arg(buffered, "state");
    if (state == NULL) {
        stdio__printf("Buffered rendering: %s\n", config__get_display_buffered_rendering() ? "on" : "off");
        return BRUCE_OK;
    }
    bool value;
    return config_app__parse_on_off(state, &value) ? config__set_display_buffered_rendering(value)
                                                   : BRUCE_ERR_INVALID_ARGUMENT;
}

static void config_app__add_gui_option(ArgParser *parser) {
    ap_add_flag(parser, "gui");
    ap_set_opt_help(parser, "gui", "Use GUI interaction mode");
}

int config_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Configure BruceOS settings.");
    config_app__add_gui_option(root);

    ArgParser *system = ap_new_cmd(root, "system");
    ArgParser *clock = system != NULL ? ap_new_cmd(system, "clock") : NULL;
    ArgParser *show = clock != NULL ? ap_new_cmd(clock, "show") : NULL;
    ArgParser *sync = clock != NULL ? ap_new_cmd(clock, "sync") : NULL;
    ArgParser *ntp = clock != NULL ? ap_new_cmd(clock, "ntp") : NULL;
    ArgParser *timezone = clock != NULL ? ap_new_cmd(clock, "timezone") : NULL;
    ArgParser *dst = clock != NULL ? ap_new_cmd(clock, "dst") : NULL;
    ArgParser *format = clock != NULL ? ap_new_cmd(clock, "format") : NULL;
    ArgParser *set = clock != NULL ? ap_new_cmd(clock, "set") : NULL;
    ArgParser *startup = ap_new_cmd(root, "startup");
    ArgParser *startup_add = startup != NULL ? ap_new_cmd(startup, "add") : NULL;
    ArgParser *startup_remove = startup != NULL ? ap_new_cmd(startup, "remove") : NULL;
    ArgParser *display = ap_new_cmd(root, "display");
    ArgParser *display_buffered = display != NULL ? ap_new_cmd(display, "buffered") : NULL;
    ArgParser *parsers[] = {
        system, clock, show, sync, ntp, timezone, dst, format, set, startup, startup_add, startup_remove,
        display, display_buffered,
    };
    for (size_t i = 0; i < sizeof(parsers) / sizeof(parsers[0]); ++i) {
        if (parsers[i] == NULL) {
            ap_free(root);
            return BRUCE_ERR_NO_MEMORY;
        }
        config_app__add_gui_option(parsers[i]);
    }

    ap_set_helptext(system, "Configure system settings.");
    ap_set_helptext(clock, "Configure the system clock and local-time display.");
    ap_set_helptext(show, "Show clock configuration and local time.");
    ap_set_helptext(sync, "Synchronize the clock from NTP now.");
    ap_set_helptext(ntp, "Enable or disable automatic NTP updates.");
    ap_add_optional_arg(ntp, "state", "on or off (required outside GUI mode)");
    ap_set_helptext(timezone, "Set the local UTC offset.");
    ap_add_optional_arg(timezone, "offset", "UTC offset from -12 to +14 (required outside GUI mode)");
    ap_unknown_options_as_args(timezone);
    ap_set_helptext(dst, "Enable or disable daylight savings.");
    ap_add_optional_arg(dst, "state", "on or off (required outside GUI mode)");
    ap_set_helptext(format, "Select 12-hour or 24-hour display.");
    ap_add_optional_arg(format, "hours", "12 or 24 (required outside GUI mode)");
    ap_set_helptext(set, "Set local date and time manually.");
    ap_add_optional_arg(set, "date", "Date as YYYY-MM-DD (required outside GUI mode)");
    ap_add_optional_arg(set, "time", "Time as HH:MM:SS (required outside GUI mode)");
    ap_set_helptext(startup, "Manage applications launched during boot.");
    ap_set_helptext(startup_add, "Append an application command to the startup list.");
    ap_add_required_arg(startup_add, "name", "Application name or quoted command line");
    ap_set_helptext(startup_remove, "Remove an application command from the startup list.");
    ap_add_required_arg(startup_remove, "name", "Application name or quoted command line");
    ap_set_helptext(display, "Configure display rendering.");
    ap_set_helptext(display_buffered, "Enable smooth buffered rendering or direct low-memory drawing.");
    ap_add_optional_arg(display_buffered, "state", "on or off");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    ArgParser *root_action = ap_get_cmd_parser(root);
    bool clock_hierarchy = root_action == system && ap_get_cmd_parser(system) == clock;
    bool startup_hierarchy = root_action == startup;
    bool display_hierarchy = root_action == display;
    bool gui = ap_found(root, "gui") || ap_found(system, "gui") || ap_found(clock, "gui") ||
               ap_found(display, "gui") || ap_found(display_buffered, "gui");
    ArgParser *action = clock_hierarchy ? ap_get_cmd_parser(clock) : NULL;
    if (action != NULL) gui = gui || ap_found(action, "gui");
    int result;
    if (clock_hierarchy) {
        result = gui ? config_app__clock_gui()
                     : config_app__clock_cli(clock, show, sync, ntp, timezone, dst, format, set);
    } else if (startup_hierarchy && !gui) {
        result = config_app__startup_cli(startup, startup_add, startup_remove);
    } else if (display_hierarchy) {
        result = gui ? config_app__display_gui() : config_app__display_cli(display, display_buffered);
    } else {
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
