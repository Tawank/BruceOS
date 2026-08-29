#include "bnu_app.h"
#include "bnu_internal.h"

#include <errno.h> // IWYU pragma: keep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/clock.h"
#include "core_sdk/device.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"

/* System commands: free, top, shutdown, reboot, stty, date, sleep. */

static void
bnu__print_memory_row(const char *name, size_t total, size_t free_size, size_t largest, bool human) {
    char total_text[16];
    char used_text[16];
    char free_text[16];
    char largest_text[16];
    bnu__format_size((uint32_t)total, human, total_text, sizeof(total_text));
    bnu__format_size((uint32_t)(total - free_size), human, used_text, sizeof(used_text));
    bnu__format_size((uint32_t)free_size, human, free_text, sizeof(free_text));
    bnu__format_size((uint32_t)largest, human, largest_text, sizeof(largest_text));
    stdio__printf("%-5s %7s %7s %6s %6s\n", name, total_text, used_text, free_text, largest_text);
}

int bnu_free_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show internal memory, PSRAM, and swap usage.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "H");
    ap_set_opt_help(parser, "H", "Show sizes in human-readable units (e.g. 8.2K, 1.3M)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool human = ap_found(parser, "H");
    ap_free(parser);
    bruce_memory_stats_t stats;
    bruce_result_t result = memory__get_stats(&stats);
    if (result != BRUCE_OK) return result;
    stdio__printf("%-5s %7s %7s %6s %6s\n", "mem", "total", "used", "free", "lrgst");
    bnu__print_memory_row(
        "int", stats.internal_total, stats.internal_free, stats.internal_largest_block, human
    );
    if (stats.psram_total > 0) {
        bnu__print_memory_row("psram", stats.psram_total, stats.psram_free, stats.psram_largest_block, human);
    }
    if (stats.swap_total > 0) {
        bnu__print_memory_row("swap", stats.swap_total, stats.swap_free, stats.swap_largest_block, human);
    }
    return BRUCE_OK;
}

static const char *bnu__process_state_name(bruce_process_state_t state) {
    switch (state) {
        case BRUCE_PROCESS_STARTING: return "start";
        case BRUCE_PROCESS_FOREGROUND: return "fore";
        case BRUCE_PROCESS_BACKGROUND: return "back";
        case BRUCE_PROCESS_PAUSED: return "pause";
        case BRUCE_PROCESS_STOPPING: return "stop";
        default: return "?";
    }
}

int bnu_top_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show runtime process resource usage.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "H");
    ap_set_opt_help(parser, "H", "Show stck/heap/swap sizes in human-readable units (e.g. 8.2K, 1.3M)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool human = ap_found(parser, "H");
    ap_free(parser);

    bruce_process_snapshot_t processes[16];
    size_t process_count = 0;
    bruce_result_t result =
        process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
    if (result != BRUCE_OK) return result;
    result = runtime__delay(250);
    if (result != BRUCE_OK) return result;
    result = process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
    if (result != BRUCE_OK) return result;

    stdio__printf("\n%1s %2s %3s %4s %4s %4s %s\n", "s", "id", "cpu", "stck", "heap", "swap", "name");
    for (size_t i = 0; i < process_count; ++i) {
        uint32_t stack_used_bytes = processes[i].stack_total_bytes > processes[i].stack_high_water_bytes
                                        ? processes[i].stack_total_bytes - processes[i].stack_high_water_bytes
                                        : 0;
        /* memory_bytes tracks everything the process owns, swap included;
         * subtract swap_bytes here so the displayed "heap" is RAM only
         * (internal heap + PSRAM) and doesn't double-count the swap column. */
        uint32_t ram_bytes = processes[i].memory_bytes > processes[i].swap_bytes
                                 ? processes[i].memory_bytes - processes[i].swap_bytes
                                 : 0;
        char stack_text[16];
        char heap_text[16];
        char swap_text[16];
        bnu__format_size(stack_used_bytes, human, stack_text, sizeof(stack_text));
        bnu__format_size(ram_bytes, human, heap_text, sizeof(heap_text));
        bnu__format_size((uint32_t)processes[i].swap_bytes, human, swap_text, sizeof(swap_text));
        stdio__printf(
            "%1.1s %2u %3u %4s %4s %4s %.15s\n",
            bnu__process_state_name(processes[i].state),
            (unsigned)processes[i].id,
            (unsigned)processes[i].cpu_percent,
            stack_text,
            heap_text,
            swap_text,
            processes[i].name
        );
    }
    return BRUCE_OK;
}

static bruce_result_t bnu__parse_shutdown_time(const char *text, uint32_t *out_delay_ms) {
    if (text == NULL || out_delay_ms == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (strcmp(text, "now") == 0) {
        *out_delay_ms = 0;
        return BRUCE_OK;
    }

    if (text[0] == '+') {
        errno = 0;
        char *end = NULL;
        unsigned long minutes = strtoul(text + 1, &end, 10);
        if (errno != 0 || end == text + 1 || *end != '\0' || minutes > UINT32_MAX / 60000u) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        *out_delay_ms = (uint32_t)minutes * 60000u;
        return BRUCE_OK;
    }

    if (strlen(text) != 5 || text[2] != ':' || text[0] < '0' || text[0] > '9' || text[1] < '0' ||
        text[1] > '9' || text[3] < '0' || text[3] > '9' || text[4] < '0' || text[4] > '9') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    unsigned hour = (unsigned)(text[0] - '0') * 10u + (unsigned)(text[1] - '0');
    unsigned minute = (unsigned)(text[3] - '0') * 10u + (unsigned)(text[4] - '0');
    if (hour > 23 || minute > 59) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_clock_datetime_t now;
    bruce_result_t result = clock__get_local(&now);
    if (result != BRUCE_OK) return result;
    int delay_minutes = (int)(hour * 60u + minute) - (int)(now.hour * 60u + now.minute);
    if (delay_minutes <= 0) delay_minutes += 24 * 60;
    *out_delay_ms = (uint32_t)delay_minutes * 60000u;
    return BRUCE_OK;
}

int bnu_shutdown_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Power off the device at the specified time.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "time", "'now', '+minutes', or 24-hour 'HH:MM'");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    uint32_t delay_ms = 0;
    bruce_result_t result = bnu__parse_shutdown_time(ap_get_arg(parser, "time"), &delay_ms);
    ap_free(parser);
    if (result != BRUCE_OK) return result;
    stdio__printf("Shutting down...\n");
    return device__power_off(delay_ms);
}

int bnu_reboot_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Restart the device.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);
    stdio__printf("Rebooting...\n");
    return device__restart(0);
}

int bnu_stty_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser(
        "Show or change the calling process's terminal settings (rows, columns, raw/cooked mode)."
    );
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_optional_arg(parser, "setting", "'size', 'raw', or '-raw'/'cooked'/'sane'");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *setting = ap_get_arg(parser, "setting");
    ap_free(parser);

    if (!tty__isatty()) {
        stdio__printf("stty: standard input is not a tty\n");
        return BRUCE_ERR_NOT_FOUND;
    }

    if (setting == NULL) {
        bruce_tty_size_t size;
        bruce_result_t result = tty__get_size(&size);
        if (result != BRUCE_OK) return result;
        stdio__printf(
            "speed 0 baud; rows %u; columns %u; line = 0;\n%s\n",
            (unsigned)size.rows,
            (unsigned)size.columns,
            tty__get_mode() == BRUCE_TTY_MODE_RAW ? "raw -echo -icanon" : "-raw echo icanon"
        );
        return BRUCE_OK;
    }
    if (strcmp(setting, "size") == 0) {
        bruce_tty_size_t size;
        bruce_result_t result = tty__get_size(&size);
        if (result != BRUCE_OK) return result;
        stdio__printf("%u %u\n", (unsigned)size.rows, (unsigned)size.columns);
        return BRUCE_OK;
    }
    if (strcmp(setting, "raw") == 0) return tty__set_mode(BRUCE_TTY_MODE_RAW);
    if (strcmp(setting, "-raw") == 0 || strcmp(setting, "cooked") == 0 || strcmp(setting, "sane") == 0) {
        return tty__set_mode(BRUCE_TTY_MODE_COOKED);
    }
    stdio__printf("stty: unknown setting '%s'\n", setting);
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static bool bnu__parse_datetime(const char *text, bruce_clock_datetime_t *out) {
    unsigned int year, month, day, hour, minute, second;
    char extra;
    if (text == NULL ||
        sscanf(text, "%u-%u-%u %u:%u:%u%c", &year, &month, &day, &hour, &minute, &second, &extra) != 6) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    out->year = (uint16_t)year;
    out->month = (uint8_t)month;
    out->day = (uint8_t)day;
    out->hour = (uint8_t)hour;
    out->minute = (uint8_t)minute;
    out->second = (uint8_t)second;
    return true;
}

int bnu_date_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show or set the current date and time.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "u");
    ap_set_opt_help(parser, "u", "Show the time in UTC instead of local time");
    ap_add_str_opt(parser, "s set", NULL);
    ap_set_opt_help(parser, "s set", "Set the date and time ('YYYY-MM-DD HH:MM:SS', local)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool utc = ap_found(parser, "u");
    const char *set_value = ap_get_str_value(parser, "s");
    bruce_clock_datetime_t set_local;
    bool has_set_value = set_value != NULL && bnu__parse_datetime(set_value, &set_local);
    bool set_requested = set_value != NULL;
    ap_free(parser);

    if (set_requested) {
        if (!has_set_value) return BRUCE_ERR_INVALID_ARGUMENT;
        bruce_result_t result = clock__set_local(&set_local);
        if (result != BRUCE_OK) return result;
    }

    bruce_clock_datetime_t now;
    bruce_result_t result = utc ? clock__get_utc(&now) : clock__get_local(&now);
    if (result != BRUCE_OK) return result;
    stdio__printf(
        "%04u-%02u-%02u %02u:%02u:%02u%s\n",
        (unsigned)now.year,
        (unsigned)now.month,
        (unsigned)now.day,
        (unsigned)now.hour,
        (unsigned)now.minute,
        (unsigned)now.second,
        utc ? " UTC" : ""
    );
    return BRUCE_OK;
}

int bnu_sleep_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Pause for the given duration.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "seconds", "Duration to sleep, in seconds (e.g. 2 or 0.5)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *text = ap_get_arg(parser, "seconds");

    errno = 0;
    char *end = NULL;
    double seconds = text != NULL ? strtod(text, &end) : 0.0;
    bool valid = text != NULL && end != text && *end == '\0' && errno == 0 && seconds >= 0.0 &&
                 seconds <= (double)UINT32_MAX / 1000.0;
    ap_free(parser);
    if (!valid) return BRUCE_ERR_INVALID_ARGUMENT;

    return runtime__sleep((uint32_t)(seconds * 1000.0));
}
