#include "config_app.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int config_app__show_clock(void) {
    bool automatic = false, dst = false, format24 = true;
    float timezone = 0;
    bruce_clock_datetime_t now;
    (void)config__get_automatic_time_update_via_ntp(&automatic);
    (void)config__get_tmz(&timezone);
    (void)config__get_dst(&dst);
    (void)config__get_clock24hr(&format24);
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
        bool automatic = false, dst = false, format24 = true;
        float timezone = 0;
        (void)config__get_automatic_time_update_via_ntp(&automatic);
        (void)config__get_tmz(&timezone);
        (void)config__get_dst(&dst);
        (void)config__get_clock24hr(&format24);
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
    if (strcmp(text, "on") == 0) *out = true;
    else if (strcmp(text, "off") == 0) *out = false;
    else return false;
    return true;
}

static int config_app__clock_cli(int argc, char **argv) {
    if (argc <= 3 || strcmp(argv[3], "show") == 0) return config_app__show_clock();
    const char *action = argv[3];
    if (strcmp(action, "sync") == 0) {
        bruce_result_t result = clock__sync_ntp(10000);
        stdio__printf(result == BRUCE_OK ? "Clock synchronized\n" : "NTP synchronization failed: %d\n", result);
        return result;
    }
    if (strcmp(action, "ntp") == 0 && argc == 5) {
        bool value;
        return config_app__parse_on_off(argv[4], &value) ? config__set_automatic_time_update_via_ntp(value) :
                                                           BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(action, "timezone") == 0 && argc == 5) {
        char *end = NULL;
        float value = strtof(argv[4], &end);
        if (end == argv[4] || *end != '\0' || value < -12.0f || value > 14.0f) return BRUCE_ERR_INVALID_ARGUMENT;
        return config__set_tmz(value);
    }
    if (strcmp(action, "dst") == 0 && argc == 5) {
        bool value;
        return config_app__parse_on_off(argv[4], &value) ? config__set_dst(value) : BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(action, "format") == 0 && argc == 5) {
        if (strcmp(argv[4], "12") == 0) return config__set_clock24hr(false);
        if (strcmp(argv[4], "24") == 0) return config__set_clock24hr(true);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(action, "set") == 0 && argc == 6) {
        bruce_clock_datetime_t value;
        if (!config_app__parse_datetime(argv[4], argv[5], &value)) return BRUCE_ERR_INVALID_ARGUMENT;
        bruce_result_t result = clock__set_local(&value);
        if (result == BRUCE_OK) (void)config__set_automatic_time_update_via_ntp(false);
        return result;
    }
    stdio__printf("config system clock [show|sync|ntp on|off|timezone OFFSET|dst on|off|format 12|24|set DATE TIME]\n");
    return BRUCE_ERR_INVALID_ARGUMENT;
}

int config_app_main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "system") == 0 && strcmp(argv[2], "clock") == 0) {
        return app_runner__args_have_gui(argc, argv) ? config_app__clock_gui() : config_app__clock_cli(argc, argv);
    }
    stdio__printf("Supported: config system clock [--gui]\n");
    return BRUCE_ERR_INVALID_ARGUMENT;
}
