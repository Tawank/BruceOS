#include "clock_app.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *clock_app__action(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--gui") != 0) return argv[i];
    }
    return "show";
}

static const char *clock_app__argument_after(int argc, char **argv, const char *action) {
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--gui") == 0) continue;
        if (found) return argv[i];
        if (strcmp(argv[i], action) == 0) found = true;
    }
    return NULL;
}

static void clock_app__format_time(const bruce_clock_datetime_t *now, char *out, size_t size) {
    bool format24 = true;
    (void)config__get_clock24hr(&format24);
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
    uint16_t primary = BRUCE_COLOR_ORANGE;
    uint16_t secondary = BRUCE_COLOR_WHITE;
    uint16_t background = BRUCE_COLOR_BLACK;
    (void)config__get_pri_color(&primary);
    (void)config__get_sec_color(&secondary);
    (void)config__get_bg_color(&background);
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    (void)display__fill_screen(background);
    (void)display__draw_rect(4, 4, width - 8, height - 8, primary);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(secondary);
    (void)display__set_text_size(1);
    (void)display__set_cursor((width - (int)strlen(title) * 8) / 2, 14);
    (void)display__print(title);
    int text_size = 4;
    while (text_size > 1 && (int)strlen(main_text) * 8 * text_size > width - 16) text_size--;
    (void)display__set_text_color(primary);
    (void)display__set_text_size((uint8_t)text_size);
    (void
    )display__set_cursor((width - (int)strlen(main_text) * 8 * text_size) / 2, (height - 16 * text_size) / 2);
    (void)display__print(main_text);
    if (footer != NULL) {
        (void)display__set_text_color(secondary);
        (void)display__set_text_size(1);
        (void)display__set_cursor((width - (int)strlen(footer) * 8) / 2, height - 24);
        (void)display__print(footer);
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
    for (;;) {
        bruce_clock_datetime_t now;
        bruce_result_t result = clock__get_local(&now);
        if (result != BRUCE_OK) {
            (void)clock_app__draw("Clock", "--:--:--", "Set time in Config");
        } else if (now.second != last_second) {
            char formatted[16];
            char date[16];
            clock_app__format_time(&now, formatted, sizeof(formatted));
            snprintf(date, sizeof(date), "%04u-%02u-%02u", now.year, now.month, now.day);
            (void)clock_app__draw(date, formatted, "OK menu / BACK");
            last_second = now.second;
        }
        int32_t code = 0;
        result = input__wait(200, &code);
        if (result == BRUCE_OK && code == BRUCE_INPUT_CODE_BACK) return BRUCE_OK;
        if (result == BRUCE_OK && code == BRUCE_INPUT_CODE_SELECT) return CLOCK_APP_OPEN_MENU;
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

static int clock_app__timer(int argc, char **argv, bool gui) {
    uint32_t duration = 0;
    const char *argument = clock_app__argument_after(argc, argv, "timer");
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

static int clock_app__alarm(int argc, char **argv, bool gui) {
    bruce_clock_datetime_t now;
    bruce_result_t result = clock__get_local(&now);
    if (result != BRUCE_OK) return result;
    uint32_t target = 0;
    const char *argument = clock_app__argument_after(argc, argv, "alarm");
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
            if (result != BRUCE_OK && result != BRUCE_ERR_TIMEOUT) return result;
        } else if (runtime__delay(200) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    if (gui) (void)dialog__message(BRUCE_DIALOG_WARNING, "Alarm", "ALARM!");
    else stdio__printf("ALARM!\n");
    return BRUCE_OK;
}

static void clock_app__usage(void) {
    stdio__printf("Clock commands:\n");
    stdio__printf("  clock show\n  clock timer HH:MM:SS\n  clock alarm HH:MM[:SS]\n");
}

static int clock_app__gui(int argc, char **argv) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Timer", .value = "timer"},
        {.label = "Alarm", .value = "alarm"},
        {.label = "Back to clock", .value = "back"},
        {.label = "Exit", .value = "exit"},
    };
    for (;;) {
        int result = clock_app__show(true);
        if (result != CLOCK_APP_OPEN_MENU) return result;

        size_t selected = 0;
        bruce_result_t choice_result = dialog__choice("Clock", "Clock tools", choices, 4, &selected, NULL);
        if (choice_result == BRUCE_ERR_CANCELLED || selected == 2) {
            (void)input__flush();
            continue;
        }
        if (choice_result != BRUCE_OK) return choice_result;
        if (selected == 3) return BRUCE_OK;

        result = selected == 0 ? clock_app__timer(argc, argv, true) : clock_app__alarm(argc, argv, true);
        if (result != BRUCE_OK) return result;
        (void)input__flush();
    }
}

int clock_app_main(int argc, char **argv) {
    bool gui = app_runner__args_have_gui(argc, argv);
    const char *action = clock_app__action(argc, argv);
    if (strcmp(action, "show") == 0) return gui ? clock_app__gui(argc, argv) : clock_app__show(false);
    if (strcmp(action, "timer") == 0) return clock_app__timer(argc, argv, gui);
    if (strcmp(action, "alarm") == 0) return clock_app__alarm(argc, argv, gui);
    clock_app__usage();
    return strcmp(action, "help") == 0 ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}
