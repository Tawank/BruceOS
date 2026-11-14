#include "clock_app.h"

#include "clock_gui.h"
#include "core_sdk/args.h"
#include "core_sdk/audio.h"
#include "core_sdk/clock.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#include <stdio.h>
#include <stdlib.h>

static bool clock_app__parse_duration(const char *text, uint32_t *out_seconds) {
    unsigned int hours, minutes, seconds;
    char extra;
    if (text == NULL || sscanf(text, "%u:%u:%u%c", &hours, &minutes, &seconds, &extra) != 3 || hours > 99 ||
        minutes > 59 || seconds > 59) {
        return false;
    }
    *out_seconds = hours * 3600u + minutes * 60u + seconds;
    return *out_seconds > 0;
}

static bool clock_app__parse_alarm(const char *text, uint32_t *out_seconds_of_day) {
    unsigned int hours, minutes, seconds = 0;
    char extra;
    int fields = text == NULL ? 0 : sscanf(text, "%u:%u:%u%c", &hours, &minutes, &seconds, &extra);
    if (fields < 2 || fields > 3 || hours > 23 || minutes > 59 || seconds > 59) return false;
    *out_seconds_of_day = hours * 3600u + minutes * 60u + seconds;
    return true;
}

static void clock_app__ring(void) {
    for (int beep = 0; beep < 3; ++beep) {
        (void)audio__tone(1500, 150, false);
        (void)runtime__delay(100);
    }
}

static int clock_app__show(void) {
    bruce_clock_datetime_t now;
    bruce_result_t result = clock__get_local(&now);
    if (result != BRUCE_OK) return result;
    stdio__printf("%04u-%02u-%02u %02u:%02u:%02u\n", now.year, now.month, now.day, now.hour, now.minute, now.second);
    return BRUCE_OK;
}

static int clock_app__timer(const char *argument) {
    uint32_t duration;
    if (!clock_app__parse_duration(argument, &duration)) return BRUCE_ERR_INVALID_ARGUMENT;
    uint64_t deadline = runtime__now() + (uint64_t)duration * 1000u;
    while (runtime__now() < deadline) {
        if (runtime__delay(100) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    stdio__printf("TIME'S UP!\n");
    clock_app__ring();
    return BRUCE_OK;
}

static int clock_app__alarm(const char *argument) {
    uint32_t target;
    if (!clock_app__parse_alarm(argument, &target)) return BRUCE_ERR_INVALID_ARGUMENT;
    for (;;) {
        bruce_clock_datetime_t now;
        if (clock__get_local(&now) != BRUCE_OK) return BRUCE_ERR_INVALID_STATE;
        if (now.hour * 3600u + now.minute * 60u + now.second == target) break;
        if (runtime__delay(200) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    stdio__printf("ALARM!\n");
    clock_app__ring();
    return BRUCE_OK;
}

static void clock_app__add_gui_option(ArgParser *parser) {
    ap_add_flag(parser, "gui");
    ap_set_opt_help(parser, "gui", "Use GUI interaction mode");
}

int clock_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Show the clock, or run timers, alarms and a stopwatch.");
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
    ap_add_optional_arg(timer, "duration", "Countdown duration as HH:MM:SS (required without --gui)");
    ap_set_helptext(alarm, "Wait until a local time of day.");
    ap_add_optional_arg(alarm, "time", "Alarm time as HH:MM[:SS] (required without --gui)");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    ArgParser *command = ap_get_cmd_parser(root);
    const char *duration_arg = command == timer ? ap_get_arg(timer, "duration") : NULL;
    const char *alarm_arg = command == alarm ? ap_get_arg(alarm, "time") : NULL;
    bool gui = runtime__is_gui_requested();
    /* Arguments point into argv, so they outlive the parser's arena. */
    ap_free(root);

    if (gui) {
        return clock_gui__run(command == timer   ? CLOCK_SCREEN_TIMERS
                              : command == alarm ? CLOCK_SCREEN_ALARMS
                                                 : CLOCK_SCREEN_FACE);
    }
    if (command == timer) return clock_app__timer(duration_arg);
    if (command == alarm) return clock_app__alarm(alarm_arg);
    return clock_app__show();
}
