#include "clock_app.h"

#include "core_sdk/args.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CLOCK_APP__MENU_HINT_MS 5000u


static bool clock_app__resume_after_handoff(void) {
    bruce_process_snapshot_t snapshot;
    bruce_process_id_t self = process__current_id();
    if (self == BRUCE_PROCESS_ID_INVALID || process__snapshot(self, &snapshot) != BRUCE_OK ||
        snapshot.state != BRUCE_PROCESS_BACKGROUND) {
        return false;
    }
    do {
        if (runtime__delay(20) != BRUCE_OK || process__snapshot(self, &snapshot) != BRUCE_OK) return false;
    } while (snapshot.state == BRUCE_PROCESS_BACKGROUND);
    return snapshot.state == BRUCE_PROCESS_FOREGROUND;
}

static void clock_app__format_time(const bruce_clock_datetime_t *now, char *out, size_t size) {
    bool format24 = config__get_time_clock24hr();
    if (format24) {
        snprintf(out, size, "%02u:%02u:%02u", now->hour, now->minute, now->second);
        return;
    }
    unsigned int hour = now->hour % 12u;
    if (hour == 0) hour = 12;
    snprintf(out, size, "%02u:%02u:%02u %s", hour, now->minute, now->second, now->hour < 12 ? "AM" : "PM");
}

static bruce_result_t clock_app__draw(const char *title, const char *main_text, const char *footer) {
    int width = display__width();
    int height = display__height();
    uint16_t primary = config__get_color_primary();
    uint16_t secondary = config__get_color_secondary();
    uint16_t background = config__get_color_background();
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    (void)display__fill_screen(background);
    (void)display__draw_rect(4, 4, width - 8, height - 8, primary);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(secondary);
    (void)display__set_text_size(1);
    (void)display__draw_centre_string(title, width / 2, 14);
    int text_size = 4;
    while (text_size > 1 && (int)strlen(main_text) * 6 * text_size > width - 16) text_size--;
    (void)display__set_text_color(primary);
    (void)display__set_text_size((uint8_t)text_size);
    int16_t font_width = 0;
    int16_t font_height = 0;
    if (display__get_font_metrics(&font_width, &font_height) != BRUCE_OK || font_height <= 0) {
        font_height = 8;
    }
    (void)display__draw_centre_string(main_text, width / 2, (height - font_height * text_size) / 2);
    if (footer != NULL) {
        (void)display__set_text_color(secondary);
        (void)display__set_text_size(1);
        (void)display__draw_centre_string(footer, width / 2, height - 24);
    }
    return display__present();
}

enum { CLOCK_APP_OPEN_MENU = 1 };

static int clock_app__show(bool gui) {
    if (!gui) {
        bruce_clock_datetime_t now;
        bruce_result_t result = clock__get_local(&now);
        if (result != BRUCE_OK) {
            dialog__message(BRUCE_DIALOG_ERROR, "Clock", "Clock is not set");
            return result;
        }
        char formatted[16];
        clock_app__format_time(&now, formatted, sizeof(formatted));
        stdio__printf("%04u-%02u-%02u %s\n", now.year, now.month, now.day, formatted);
        return BRUCE_OK;
    }
    (void)input__flush();
    uint8_t last_second = UINT8_MAX;
    uint64_t hint_started_at = runtime__now();
    bool last_hint_visible = false;
    for (;;) {
        bool hint_visible = runtime__now() - hint_started_at < CLOCK_APP__MENU_HINT_MS;
        bruce_clock_datetime_t now;
        bruce_result_t result = clock__get_local(&now);
        if (result != BRUCE_OK) {
            (void)clock_app__draw("Clock", "--:--:--", "Set time in Config");
        } else if (now.second != last_second || hint_visible != last_hint_visible) {
            char formatted[16];
            char date[16];
            clock_app__format_time(&now, formatted, sizeof(formatted));
            snprintf(date, sizeof(date), "%04u-%02u-%02u", now.year, now.month, now.day);
            (void)clock_app__draw(date, formatted, hint_visible ? "OK to show menu" : NULL);
            last_second = now.second;
            last_hint_visible = hint_visible;
        }
        int32_t code = 0;
        result = input__wait(200, &code);
        if (result == BRUCE_OK && code == BRUCE_INPUT_CODE_BACK) return BRUCE_OK;
        if (result == BRUCE_OK && code == BRUCE_INPUT_CODE_SELECT) return CLOCK_APP_OPEN_MENU;
        if (result == BRUCE_ERR_NOT_FOREGROUND && clock_app__resume_after_handoff()) continue;
        if (result != BRUCE_OK && result != BRUCE_ERR_TIMEOUT) return result;
    }
}

static bool clock_app__parse_duration(const char *text, uint32_t *out_seconds) {
    unsigned int hours, minutes, seconds;
    char extra;
    if (text == NULL || sscanf(text, "%u:%u:%u%c", &hours, &minutes, &seconds, &extra) != 3 || hours > 99 ||
        minutes > 59 || seconds > 59) {
        return false;
    }
    uint32_t total = hours * 3600u + minutes * 60u + seconds;
    if (total == 0) return false;
    *out_seconds = total;
    return true;
}

static bruce_result_t clock_app__prompt_duration(uint32_t *out_seconds) {
    char hours_text[4] = "0";
    char minutes_text[4] = "0";
    char seconds_text[4] = "0";
    if (dialog__number_input("Timer", "Hours (0-99)", "0", hours_text, sizeof(hours_text)) != BRUCE_OK ||
        dialog__number_input("Timer", "Minutes (0-59)", "0", minutes_text, sizeof(minutes_text)) !=
            BRUCE_OK ||
        dialog__number_input("Timer", "Seconds (0-59)", "0", seconds_text, sizeof(seconds_text)) !=
            BRUCE_OK) {
        return BRUCE_ERR_CANCELLED;
    }
    char duration[16];
    snprintf(duration, sizeof(duration), "%s:%s:%s", hours_text, minutes_text, seconds_text);
    return clock_app__parse_duration(duration, out_seconds) ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}

static int clock_app__timer(const char *argument, bool gui) {
    uint32_t duration = 0;
    bruce_result_t result =
        argument != NULL
            ? (clock_app__parse_duration(argument, &duration) ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT)
            : (gui ? clock_app__prompt_duration(&duration) : BRUCE_ERR_INVALID_ARGUMENT);
    if (result != BRUCE_OK) {
        if (gui && result == BRUCE_ERR_INVALID_ARGUMENT)
            (void)dialog__message(BRUCE_DIALOG_ERROR, "Timer", "Enter a non-zero time within 99:59:59");
        return result == BRUCE_ERR_CANCELLED ? BRUCE_OK : result;
    }
    uint64_t deadline = runtime__now() + (uint64_t)duration * 1000u;
    uint32_t last_remaining = UINT32_MAX;
    (void)input__flush();
    while (runtime__now() < deadline) {
        uint64_t milliseconds = deadline - runtime__now();
        uint32_t remaining = (uint32_t)((milliseconds + 999u) / 1000u);
        if (remaining != last_remaining) {
            char formatted[16];
            snprintf(
                formatted,
                sizeof(formatted),
                "%02lu:%02lu:%02lu",
                (unsigned long)(remaining / 3600u),
                (unsigned long)(remaining / 60u % 60u),
                (unsigned long)(remaining % 60u)
            );
            if (gui) (void)clock_app__draw("Timer", formatted, "BACK to cancel");
            else stdio__printf("%s\n", formatted);
            last_remaining = remaining;
        }
        if (gui) {
            int32_t code = 0;
            result = input__wait(100, &code);
            if (result == BRUCE_OK && code == BRUCE_INPUT_CODE_BACK) return BRUCE_OK;
            if (result == BRUCE_ERR_NOT_FOREGROUND && clock_app__resume_after_handoff()) continue;
            if (result != BRUCE_OK && result != BRUCE_ERR_TIMEOUT) return result;
        } else if (runtime__delay(100) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    if (gui) (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Timer", "TIME'S UP!");
    else stdio__printf("TIME'S UP!\n");
    return BRUCE_OK;
}

static bool clock_app__parse_alarm(const char *text, uint32_t *out_seconds_of_day) {
    unsigned int hours, minutes, seconds = 0;
    char extra;
    int fields = text == NULL ? 0 : sscanf(text, "%u:%u:%u%c", &hours, &minutes, &seconds, &extra);
    if (fields < 2 || fields > 3 || hours > 23 || minutes > 59 || seconds > 59) return false;
    *out_seconds_of_day = hours * 3600u + minutes * 60u + seconds;
    return true;
}

static bruce_result_t clock_app__prompt_alarm(uint32_t *out_seconds_of_day) {
    char hours[4] = "0";
    char minutes[4] = "0";
    if (dialog__number_input("Alarm", "Hour (0-23)", "0", hours, sizeof(hours)) != BRUCE_OK ||
        dialog__number_input("Alarm", "Minute (0-59)", "0", minutes, sizeof(minutes)) != BRUCE_OK) {
        return BRUCE_ERR_CANCELLED;
    }
    char value[12];
    snprintf(value, sizeof(value), "%s:%s", hours, minutes);
    return clock_app__parse_alarm(value, out_seconds_of_day) ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}

static int clock_app__alarm(const char *argument, bool gui) {
    bruce_clock_datetime_t now;
    bruce_result_t result = clock__get_local(&now);
    if (result != BRUCE_OK) return result;
    uint32_t target = 0;
    result = argument != NULL
                 ? (clock_app__parse_alarm(argument, &target) ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT)
                 : (gui ? clock_app__prompt_alarm(&target) : BRUCE_ERR_INVALID_ARGUMENT);
    if (result != BRUCE_OK) return result == BRUCE_ERR_CANCELLED ? BRUCE_OK : result;
    uint8_t last_second = UINT8_MAX;
    (void)input__flush();
    for (;;) {
        if (clock__get_local(&now) != BRUCE_OK) return BRUCE_ERR_INVALID_STATE;
        uint32_t local_seconds = now.hour * 3600u + now.minute * 60u + now.second;
        if (local_seconds == target) break;
        if (gui && now.second != last_second) {
            char message[16];
            snprintf(
                message,
                sizeof(message),
                "%02lu:%02lu:%02lu",
                (unsigned long)(target / 3600u),
                (unsigned long)(target / 60u % 60u),
                (unsigned long)(target % 60u)
            );
            (void)clock_app__draw("Alarm set", message, "BACK to cancel");
            last_second = now.second;
        }
        if (gui) {
            int32_t code = 0;
            result = input__wait(200, &code);
            if (result == BRUCE_OK && code == BRUCE_INPUT_CODE_BACK) return BRUCE_OK;
            if (result == BRUCE_ERR_NOT_FOREGROUND && clock_app__resume_after_handoff()) continue;
            if (result != BRUCE_OK && result != BRUCE_ERR_TIMEOUT) return result;
        } else if (runtime__delay(200) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    if (gui) (void)dialog__message(BRUCE_DIALOG_WARNING, "Alarm", "ALARM!");
    else stdio__printf("ALARM!\n");
    return BRUCE_OK;
}

static void clock_app__add_gui_option(ArgParser *parser) {
    ap_add_flag(parser, "gui");
    ap_set_opt_help(parser, "gui", "Use GUI interaction mode");
}

static int clock_app__gui(void) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Timer",         .value = "timer"},
        {.label = "Alarm",         .value = "alarm"},
        {.label = "Back to clock", .value = "back" },
        {.label = "Exit",          .value = "exit" },
    };
    for (;;) {
        int result = clock_app__show(true);
        if (result != CLOCK_APP_OPEN_MENU) return result;

        size_t selected = 0;
        bruce_result_t choice_result = dialog__choice("Clock", "Clock tools", choices, 4, &selected);
        if (choice_result == BRUCE_ERR_CANCELLED && clock_app__resume_after_handoff()) {
            (void)input__flush();
            continue;
        }
        if (choice_result == BRUCE_ERR_CANCELLED) {
            (void)input__flush();
            continue;
        }
        if (choice_result != BRUCE_OK) return choice_result;
        const char *action = choices[selected].value;
        if (strcmp(action, "back") == 0) {
            (void)input__flush();
            continue;
        }
        if (strcmp(action, "exit") == 0) return BRUCE_OK;

        result = strcmp(action, "timer") == 0 ? clock_app__timer(NULL, true) : clock_app__alarm(NULL, true);
        if (result != BRUCE_OK) return result;
        (void)input__flush();
    }
}

int clock_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Show the clock or run timer and alarm tools.");
    clock_app__add_gui_option(root);

    ArgParser *show = ap_new_cmd(root, "show");
    ArgParser *timer = ap_new_cmd(root, "timer");
    ArgParser *alarm = ap_new_cmd(root, "alarm");
    if (show == NULL || timer == NULL || alarm == NULL) {
        ap_free(root);
        return BRUCE_ERR_NO_MEMORY;
    }
    clock_app__add_gui_option(show);
    clock_app__add_gui_option(timer);
    clock_app__add_gui_option(alarm);
    ap_set_helptext(show, "Show the current local date and time.");
    ap_set_helptext(timer, "Run a countdown timer.");
    ap_add_optional_arg(timer, "duration", "Countdown duration as HH:MM:SS (GUI prompts if omitted)");
    ap_set_helptext(alarm, "Wait until a local time of day.");
    ap_add_optional_arg(alarm, "time", "Alarm time as HH:MM[:SS] (GUI prompts if omitted)");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    ArgParser *command = ap_get_cmd_parser(root);
    bool gui = runtime__gui_requested();
    bool run_show = command == NULL || command == show;
    bool run_timer = command == timer;
    const char *duration_arg = run_timer ? ap_get_arg(timer, "duration") : NULL;
    const char *alarm_arg = !run_show && !run_timer ? ap_get_arg(alarm, "time") : NULL;
    /* These point into argv, not the arena, so they stay valid after
     * ap_free() below -- only the parser's fixed 8KB arena needs to be
     * released here, before the potentially long-running loop that follows,
     * instead of being held for the app's whole lifetime. */
    ap_free(root);

    if (run_show) return gui ? clock_app__gui() : clock_app__show(false);
    if (run_timer) return clock_app__timer(duration_arg, gui);
    return clock_app__alarm(alarm_arg, gui);
}
