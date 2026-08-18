#include "config_app.h"

#include "args.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "theme_presets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int config_app__show_clock(void) {
    bool automatic = config__get_time_automatic_update_via_ntp();
    bool dst = config__get_time_dst();
    bool format24 = config__get_time_clock24hr();
    float timezone = config__get_time_timezone();
    bruce_clock_datetime_t now;
    bruce_result_t clock_result = clock__get_local(&now);
    if (clock_result == BRUCE_OK) {
        stdio__printf(
            "Local time: %04u-%02u-%02u %02u:%02u:%02u\n",
            now.year,
            now.month,
            now.day,
            now.hour,
            now.minute,
            now.second
        );
    } else stdio__printf("Local time: not set\n");
    stdio__printf("NTP: %s (%s)\n", automatic ? "automatic" : "manual", clock__get_ntp_server());
    stdio__printf(
        "Timezone: UTC%+.2f\nDST: %s\nFormat: %s\n",
        timezone,
        dst ? "on" : "off",
        format24 ? "24-hour" : "12-hour"
    );
    return BRUCE_OK;
}

static bool config_app__parse_datetime(const char *date, const char *time, bruce_clock_datetime_t *out) {
    unsigned int year, month, day, hour, minute, second;
    char extra;
    if (date == NULL || time == NULL || sscanf(date, "%u-%u-%u%c", &year, &month, &day, &extra) != 3 ||
        sscanf(time, "%u:%u:%u%c", &hour, &minute, &second, &extra) != 3)
        return false;
    *out = (bruce_clock_datetime_t){year, month, day, hour, minute, second};
    return true;
}

static bruce_result_t config_app__manual_dialog(void) {
    bruce_clock_datetime_t now = {2026, 1, 1, 0, 0, 0};
    (void)clock__get_local(&now);
    char initial_date[16], initial_time[16], date[16], time[16];
    snprintf(initial_date, sizeof(initial_date), "%04u-%02u-%02u", now.year, now.month, now.day);
    snprintf(initial_time, sizeof(initial_time), "%02u:%02u:%02u", now.hour, now.minute, now.second);
    if (dialog__text_input("Manual clock", "Date YYYY-MM-DD", initial_date, false, date, sizeof(date)) !=
            BRUCE_OK ||
        dialog__text_input("Manual clock", "Time HH:MM:SS", initial_time, false, time, sizeof(time)) !=
            BRUCE_OK) {
        return BRUCE_ERR_CANCELLED;
    }
    bruce_clock_datetime_t value;
    if (!config_app__parse_datetime(date, time, &value)) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = clock__set_local(&value);
    if (result == BRUCE_OK) (void)config__set_time_automatic_update_via_ntp(false);
    return result;
}

static int config_app__clock_gui(void) {
    for (;;) {
        bool automatic = config__get_time_automatic_update_via_ntp();
        bool dst = config__get_time_dst();
        bool format24 = config__get_time_clock24hr();
        float timezone = config__get_time_timezone();
        char ntp_label[40], timezone_label[40], dst_label[32], format_label[32];
        snprintf(ntp_label, sizeof(ntp_label), "Automatic NTP: %s", automatic ? "ON" : "OFF");
        snprintf(timezone_label, sizeof(timezone_label), "Timezone: UTC%+.2f", timezone);
        snprintf(dst_label, sizeof(dst_label), "Daylight savings: %s", dst ? "ON" : "OFF");
        snprintf(format_label, sizeof(format_label), "Clock format: %s", format24 ? "24-hour" : "12-hour");
        const bruce_dialog_choice_t choices[] = {
            {.label = "Sync from NTP now",          .value = "sync"     },
            {.label = ntp_label,                    .value = "automatic"},
            {.label = timezone_label,               .value = "timezone" },
            {.label = dst_label,                    .value = "dst"      },
            {.label = format_label,                 .value = "format"   },
            {.label = "Set date and time manually", .value = "manual"   },
            {.label = "Back",                       .value = "back"     },
        };
        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher("System clock", NULL, choices, 7, &selected);
        if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
        if (result != BRUCE_OK) return result;
        const char *action = choices[selected].value;
        if (strcmp(action, "back") == 0) return BRUCE_OK;
        if (strcmp(action, "sync") == 0) {
            result = clock__sync_ntp(10000);
            (void)dialog__message(
                result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_ERROR,
                "NTP sync",
                result == BRUCE_OK ? "Clock synchronized" : "Connect Wi-Fi and try again"
            );
        } else if (strcmp(action, "automatic") == 0) {
            (void)config__set_time_automatic_update_via_ntp(!automatic);
        } else if (strcmp(action, "timezone") == 0) {
            char initial[16], entered[16];
            snprintf(initial, sizeof(initial), "%.2f", timezone);
            if (dialog__number_input(
                    "Timezone", "UTC offset (-12 to +14)", initial, entered, sizeof(entered)
                ) == BRUCE_OK) {
                char *end = NULL;
                float value = strtof(entered, &end);
                if (end != entered && *end == '\0' && value >= -12.0f && value <= 14.0f)
                    (void)config__set_time_timezone(value);
                else (void)dialog__message(BRUCE_DIALOG_ERROR, "Timezone", "Offset must be from -12 to +14");
            }
        } else if (strcmp(action, "dst") == 0) {
            (void)config__set_time_dst(!dst);
        } else if (strcmp(action, "format") == 0) {
            (void)config__set_time_clock24hr(!format24);
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

static int config_app__clock_cli(
    ArgParser *clock_parser, ArgParser *show, ArgParser *sync, ArgParser *ntp, ArgParser *timezone,
    ArgParser *dst, ArgParser *format, ArgParser *set
) {
    ArgParser *action = ap_get_cmd_parser(clock_parser);
    if (action == NULL || action == show) return config_app__show_clock();
    if (action == sync) {
        bruce_result_t result = clock__sync_ntp(10000);
        stdio__printf(
            result == BRUCE_OK ? "Clock synchronized\n" : "NTP synchronization failed: %d\n", result
        );
        return result;
    }
    if (action == ntp) {
        bool value;
        return config_app__parse_on_off(ap_get_arg(ntp, "state"), &value)
                   ? config__set_time_automatic_update_via_ntp(value)
                   : BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (action == timezone) {
        const char *offset = ap_get_arg(timezone, "offset");
        if (offset == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
        char *end = NULL;
        float value = strtof(offset, &end);
        if (end == offset || *end != '\0' || value < -12.0f || value > 14.0f)
            return BRUCE_ERR_INVALID_ARGUMENT;
        return config__set_time_timezone(value);
    }
    if (action == dst) {
        bool value;
        return config_app__parse_on_off(ap_get_arg(dst, "state"), &value) ? config__set_time_dst(value)
                                                                          : BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (action == format) {
        const char *value = ap_get_arg(format, "hours");
        if (value == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
        if (strcmp(value, "12") == 0) return config__set_time_clock24hr(false);
        if (strcmp(value, "24") == 0) return config__set_time_clock24hr(true);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (action == set) {
        bruce_clock_datetime_t value;
        if (!config_app__parse_datetime(ap_get_arg(set, "date"), ap_get_arg(set, "time"), &value))
            return BRUCE_ERR_INVALID_ARGUMENT;
        bruce_result_t result = clock__set_local(&value);
        if (result == BRUCE_OK) (void)config__set_time_automatic_update_via_ntp(false);
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

/* Returns the index into `values` closest to `current`, for pre-selecting a choice dialog. */
static size_t config_app__closest_index(const int *values, size_t count, int current) {
    size_t best = 0;
    int best_diff = abs(values[0] - current);
    for (size_t i = 1; i < count; ++i) {
        int diff = abs(values[i] - current);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

/* display__set_brightness() re-derives the percent it persists from the 0-255
 * byte value it was given, which truncates (e.g. 75% -> byte 191 -> back to
 * 74%). Re-apply the exact percent afterwards so the persisted setting
 * matches what was actually requested; the backlight itself is set to the
 * closest achievable byte value (display__set_brightness() also floors very
 * low nonzero requests so they stay visibly lit). */
static bruce_result_t config_app__set_brightness_percent(int percent) {
    bruce_result_t result = display__set_brightness((uint8_t)(percent * 255 / 100));
    if (result == BRUCE_OK) result = config__set_display_brightness(percent);
    return result;
}

/* Persists the rotation and, best-effort, applies it live via
 * display__set_rotation() so a GUI process doesn't need a reboot to see it.
 * Live apply can fail (e.g. no display context yet, mid-frame, or no display
 * subsystem at all when called from a headless CLI) without that being a
 * real error -- the persisted setting still took, it just won't show up
 * until the next display init. `out_live_applied`, if non-NULL, reports
 * whether the live change actually took effect. */
static bruce_result_t config_app__set_rotation(int turns, bool *out_live_applied) {
    bruce_result_t result = config__set_display_rotation(turns);
    bool live_applied = result == BRUCE_OK && display__set_rotation((uint8_t)turns) == BRUCE_OK;
    if (out_live_applied != NULL) *out_live_applied = live_applied;
    return result;
}

static int config_app__display_gui(void) {
    static const int brightness_percents[5] = {100, 75, 50, 25, 1};
    static const char *const brightness_labels[5] = {"100%", "75%", "50%", "25%", "1%"};
    static const int dim_seconds[5] = {10, 20, 30, 60, 0};
    static const char *const dim_labels[5] = {"10s", "20s", "30s", "60s", "Disabled"};
    static const char *const rotation_labels[4] = {
        "0 deg (portrait)",
        "90 deg (landscape)",
        "180 deg (portrait)",
        "270 deg (landscape)",
    };
    for (;;) {
        bool buffered = config__get_display_buffered_rendering();
        int brightness = config__get_display_brightness();
        int dim_timeout = config__get_display_dim_timeout();
        int rotation = config__get_display_rotation() & 3;
        char *brightness_label = memory__malloc(32);
        char *dim_label = memory__malloc(32);
        char *rotation_label = memory__malloc(48);
        char *buffered_label = memory__malloc(48);
        bruce_dialog_choice_t *choices = memory__calloc(5, sizeof(*choices));
        if (brightness_label == NULL || dim_label == NULL || rotation_label == NULL ||
            buffered_label == NULL || choices == NULL) {
            memory__free(brightness_label);
            memory__free(dim_label);
            memory__free(rotation_label);
            memory__free(buffered_label);
            memory__free(choices);
            return BRUCE_ERR_NO_MEMORY;
        }
        snprintf(brightness_label, 32, "Brightness: %d%%", brightness);
        if (dim_timeout > 0) snprintf(dim_label, 32, "Dim time: %ds", dim_timeout);
        else snprintf(dim_label, 32, "Dim time: Disabled");
        snprintf(rotation_label, 48, "Orientation: %s", rotation_labels[rotation]);
        snprintf(buffered_label, 48, "DMA Buf (64kB): %s", buffered ? "ON" : "OFF");
        choices[0] = (bruce_dialog_choice_t){.label = brightness_label, .value = "brightness"};
        choices[1] = (bruce_dialog_choice_t){.label = dim_label, .value = "dim"};
        choices[2] = (bruce_dialog_choice_t){.label = rotation_label, .value = "rotation"};
        choices[3] = (bruce_dialog_choice_t){.label = buffered_label, .value = "buffered"};
        choices[4] = (bruce_dialog_choice_t){.label = "Back", .value = "back"};
        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher("Display & UI", NULL, choices, 5, &selected);
        const char *action = result == BRUCE_OK && selected < 5 ? choices[selected].value : "back";
        bool back = result == BRUCE_ERR_CANCELLED || (result == BRUCE_OK && strcmp(action, "back") == 0);
        bool reboot_notice = false;
        if (!back && result == BRUCE_OK) {
            if (strcmp(action, "brightness") == 0) {
                bruce_dialog_choice_t brightness_choices[5];
                for (size_t i = 0; i < 5; ++i)
                    brightness_choices[i] =
                        (bruce_dialog_choice_t){.label = brightness_labels[i], .value = brightness_labels[i]};
                size_t brightness_selected = config_app__closest_index(brightness_percents, 5, brightness);
                bruce_result_t brightness_result = dialog__choice_launcher(
                    "Brightness", NULL, brightness_choices, 5, &brightness_selected
                );
                if (brightness_result == BRUCE_OK && brightness_selected < 5)
                    result = config_app__set_brightness_percent(brightness_percents[brightness_selected]);
            } else if (strcmp(action, "dim") == 0) {
                bruce_dialog_choice_t dim_choices[5];
                for (size_t i = 0; i < 5; ++i)
                    dim_choices[i] = (bruce_dialog_choice_t){.label = dim_labels[i], .value = dim_labels[i]};
                size_t dim_selected = config_app__closest_index(dim_seconds, 5, dim_timeout);
                bruce_result_t dim_result =
                    dialog__choice_launcher("Dim time", NULL, dim_choices, 5, &dim_selected);
                if (dim_result == BRUCE_OK && dim_selected < 5)
                    result = config__set_display_dim_timeout(dim_seconds[dim_selected]);
            } else if (strcmp(action, "rotation") == 0) {
                bruce_dialog_choice_t rotation_choices[4];
                for (size_t i = 0; i < 4; ++i)
                    rotation_choices[i] =
                        (bruce_dialog_choice_t){.label = rotation_labels[i], .value = rotation_labels[i]};
                size_t rotation_selected = (size_t)rotation;
                bruce_result_t rotation_result =
                    dialog__choice_launcher("Orientation", NULL, rotation_choices, 4, &rotation_selected);
                if (rotation_result == BRUCE_OK && rotation_selected < 4) {
                    bool live_applied = false;
                    result = config_app__set_rotation((int)rotation_selected, &live_applied);
                    reboot_notice = result == BRUCE_OK && !live_applied;
                }
            } else if (strcmp(action, "buffered") == 0) {
                result = config__set_display_buffered_rendering(!buffered);
                reboot_notice = true;
            }
        }
        memory__free(brightness_label);
        memory__free(dim_label);
        memory__free(rotation_label);
        memory__free(buffered_label);
        memory__free(choices);
        if (back) return BRUCE_OK;
        if (result != BRUCE_OK) return result;
        if (reboot_notice)
            (void)dialog__message(BRUCE_DIALOG_INFO, "Display & UI", "Setting saved; reboot to apply");
    }
}

static int config_app__display_cli(
    ArgParser *display_parser, ArgParser *buffered, ArgParser *brightness, ArgParser *dim, ArgParser *rotation
) {
    ArgParser *action = ap_get_cmd_parser(display_parser);
    if (action == NULL) {
        int dim_timeout = config__get_display_dim_timeout();
        stdio__printf(
            "Buffered rendering: %s (reboot required after changes)\n"
            "Brightness: %d%%\n"
            "Dim time: %ds (0 = off)\n"
            "Orientation: %d deg\n",
            config__get_display_buffered_rendering() ? "on" : "off",
            config__get_display_brightness(),
            dim_timeout,
            config__get_display_rotation() * 90
        );
        return BRUCE_OK;
    }
    if (action == buffered) {
        const char *state = ap_get_arg(buffered, "state");
        if (state == NULL) {
            stdio__printf(
                "Buffered rendering: %s\n", config__get_display_buffered_rendering() ? "on" : "off"
            );
            return BRUCE_OK;
        }
        bool value;
        return config_app__parse_on_off(state, &value) ? config__set_display_buffered_rendering(value)
                                                        : BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (action == brightness) {
        const char *percent = ap_get_arg(brightness, "percent");
        if (percent == NULL) {
            stdio__printf("Brightness: %d%%\n", config__get_display_brightness());
            return BRUCE_OK;
        }
        char *end = NULL;
        long value = strtol(percent, &end, 10);
        if (end == percent || *end != '\0' || value < 0 || value > 100) return BRUCE_ERR_INVALID_ARGUMENT;
        return config_app__set_brightness_percent((int)value);
    }
    if (action == dim) {
        const char *seconds = ap_get_arg(dim, "seconds");
        if (seconds == NULL) {
            stdio__printf("Dim time: %ds (0 = off)\n", config__get_display_dim_timeout());
            return BRUCE_OK;
        }
        char *end = NULL;
        long value = strtol(seconds, &end, 10);
        if (end == seconds || *end != '\0' || value < 0 || value > 60) return BRUCE_ERR_INVALID_ARGUMENT;
        return config__set_display_dim_timeout((int)value);
    }
    if (action == rotation) {
        const char *degrees = ap_get_arg(rotation, "degrees");
        if (degrees == NULL) {
            stdio__printf("Orientation: %d deg\n", config__get_display_rotation() * 90);
            return BRUCE_OK;
        }
        int turns;
        if (strcmp(degrees, "0") == 0) turns = 0;
        else if (strcmp(degrees, "90") == 0) turns = 1;
        else if (strcmp(degrees, "180") == 0) turns = 2;
        else if (strcmp(degrees, "270") == 0) turns = 3;
        else return BRUCE_ERR_INVALID_ARGUMENT;
        return config_app__set_rotation(turns, NULL);
    }
    return BRUCE_ERR_INVALID_ARGUMENT;
}

/* Named theme selection. Core (core_sdk/config.h) only stores and validates
 * the ten color_* roles; the catalog of names in theme_presets.h and the
 * UI/CLI to pick one are this module's job. */

static bruce_result_t config_app__theme_apply_preset(const config_app__theme_preset_t *preset) {
    bruce_result_t result = config__set_color_primary(preset->primary);
    if (result == BRUCE_OK) result = config__set_color_secondary(preset->secondary);
    if (result == BRUCE_OK) result = config__set_color_background(preset->background);
    if (result == BRUCE_OK) result = config__set_color_surface(preset->surface);
    if (result == BRUCE_OK) result = config__set_color_text(preset->text);
    if (result == BRUCE_OK) result = config__set_color_text_muted(preset->text_muted);
    if (result == BRUCE_OK) result = config__set_color_border(preset->border);
    if (result == BRUCE_OK) result = config__set_color_success(preset->success);
    if (result == BRUCE_OK) result = config__set_color_warning(preset->warning);
    if (result == BRUCE_OK) result = config__set_color_error(preset->error);
    return result;
}

/* Case-insensitive, and treats '-'/'_' as interchangeable with a space, so
 * "Dark Gray", "dark-gray", and "dark_gray" all match the same preset --
 * that lets the CLI form skip shell-quoting a multi-word name. */
static bool config_app__theme_name_matches(const char *a, const char *b) {
    for (;;) {
        char ca = *a, cb = *b;
        if (ca == '-' || ca == '_') ca = ' ';
        if (cb == '-' || cb == '_') cb = ' ';
        if (tolower((unsigned char)ca) != tolower((unsigned char)cb)) return false;
        if (ca == '\0') return true;
        a++;
        b++;
    }
}

static const config_app__theme_preset_t *config_app__find_theme_preset(const char *name) {
    if (name == NULL) return NULL;
    for (size_t i = 0; i < CONFIG_APP__THEME_PRESET_COUNT; ++i) {
        if (config_app__theme_name_matches(name, CONFIG_APP__THEME_PRESETS[i].name)) {
            return &CONFIG_APP__THEME_PRESETS[i];
        }
    }
    return NULL;
}

static int config_app__theme_list_cli(void) {
    for (size_t i = 0; i < CONFIG_APP__THEME_PRESET_COUNT; ++i) {
        stdio__printf("%s\n", CONFIG_APP__THEME_PRESETS[i].name);
    }
    return BRUCE_OK;
}

static int config_app__theme_show_colors(void) {
    stdio__printf(
        "primary=%04x secondary=%04x background=%04x\n",
        config__get_color_primary(),
        config__get_color_secondary(),
        config__get_color_background()
    );
    stdio__printf(
        "surface=%04x text=%04x textMuted=%04x border=%04x\n",
        config__get_color_surface(),
        config__get_color_text(),
        config__get_color_text_muted(),
        config__get_color_border()
    );
    stdio__printf(
        "success=%04x warning=%04x error=%04x\n",
        config__get_color_success(),
        config__get_color_warning(),
        config__get_color_error()
    );
    return BRUCE_OK;
}

static int config_app__theme_set_cli(const char *name) {
    const config_app__theme_preset_t *preset = config_app__find_theme_preset(name);
    if (preset == NULL) {
        stdio__printf("theme: unknown preset '%s' (see 'config theme list')\n", name != NULL ? name : "");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = config_app__theme_apply_preset(preset);
    if (result != BRUCE_OK) stdio__printf("theme: failed to apply '%s' (%d)\n", preset->name, result);
    return result;
}

/* `role` is one of the ten color_* names in core_sdk/config.h (textMuted may
 * also be written text-muted/text_muted); `hex` is anything
 * config__parse_theme_color() accepts. */
static bruce_result_t config_app__theme_set_color(const char *role, const char *hex) {
    uint16_t value;
    if (role == NULL || !config__parse_theme_color(hex, &value)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (strcasecmp(role, "primary") == 0) return config__set_color_primary(value);
    if (strcasecmp(role, "secondary") == 0) return config__set_color_secondary(value);
    if (strcasecmp(role, "background") == 0) return config__set_color_background(value);
    if (strcasecmp(role, "surface") == 0) return config__set_color_surface(value);
    if (strcasecmp(role, "text") == 0) return config__set_color_text(value);
    if (strcasecmp(role, "textmuted") == 0 || strcasecmp(role, "text-muted") == 0 ||
        strcasecmp(role, "text_muted") == 0)
        return config__set_color_text_muted(value);
    if (strcasecmp(role, "border") == 0) return config__set_color_border(value);
    if (strcasecmp(role, "success") == 0) return config__set_color_success(value);
    if (strcasecmp(role, "warning") == 0) return config__set_color_warning(value);
    if (strcasecmp(role, "error") == 0) return config__set_color_error(value);
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static int config_app__theme_set_color_cli(const char *role, const char *hex) {
    bruce_result_t result = config_app__theme_set_color(role, hex);
    if (result != BRUCE_OK) stdio__printf("theme: unknown role or invalid color\n");
    return result;
}

static int config_app__theme_gui(void) {
    for (;;) {
        size_t capacity = CONFIG_APP__THEME_PRESET_COUNT + 1;
        bruce_dialog_choice_t *choices = memory__calloc(capacity, sizeof(*choices));
        if (choices == NULL) return BRUCE_ERR_NO_MEMORY;
        size_t count = 0;
        for (; count < CONFIG_APP__THEME_PRESET_COUNT; ++count) {
            choices[count] = (bruce_dialog_choice_t){
                .label = CONFIG_APP__THEME_PRESETS[count].name, .value = CONFIG_APP__THEME_PRESETS[count].name
            };
        }
        choices[count] = (bruce_dialog_choice_t){.label = "Back", .value = "back"};
        count++;
        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher("Theme", NULL, choices, count, &selected);
        bool back = result == BRUCE_ERR_CANCELLED ||
                    (result == BRUCE_OK && strcmp(choices[selected].value, "back") == 0);
        if (!back && result == BRUCE_OK) {
            const config_app__theme_preset_t *preset = config_app__find_theme_preset(choices[selected].value);
            if (preset != NULL) result = config_app__theme_apply_preset(preset);
        }
        memory__free(choices);
        if (back) return BRUCE_OK;
        if (result != BRUCE_OK) return result;
    }
}

int config_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Configure BruceOS settings.");

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
    ArgParser *display_brightness = display != NULL ? ap_new_cmd(display, "brightness") : NULL;
    ArgParser *display_dim = display != NULL ? ap_new_cmd(display, "dim") : NULL;
    ArgParser *display_rotation = display != NULL ? ap_new_cmd(display, "rotation") : NULL;
    ArgParser *theme = ap_new_cmd(root, "theme");
    ArgParser *theme_list = theme != NULL ? ap_new_cmd(theme, "list") : NULL;
    ArgParser *theme_set = theme != NULL ? ap_new_cmd(theme, "set") : NULL;
    ArgParser *theme_get = theme != NULL ? ap_new_cmd(theme, "get") : NULL;
    ArgParser *theme_set_color = theme != NULL ? ap_new_cmd(theme, "set-color") : NULL;
    ArgParser *parsers[] = {
        system,
        clock,
        show,
        sync,
        ntp,
        timezone,
        dst,
        format,
        set,
        startup,
        startup_add,
        startup_remove,
        display,
        display_buffered,
        display_brightness,
        display_dim,
        display_rotation,
        theme,
        theme_list,
        theme_set,
        theme_get,
        theme_set_color,
    };
    for (size_t i = 0; i < sizeof(parsers) / sizeof(parsers[0]); ++i) {
        if (parsers[i] == NULL) {
            ap_free(root);
            return BRUCE_ERR_NO_MEMORY;
        }
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
    ap_set_helptext(display_brightness, "Set the backlight brightness.");
    ap_add_optional_arg(display_brightness, "percent", "0 to 100 (required outside GUI mode)");
    ap_set_helptext(display_dim, "Set the idle time before the display dims.");
    ap_add_optional_arg(display_dim, "seconds", "0 to 60, 0 = off (required outside GUI mode)");
    ap_set_helptext(display_rotation, "Set the display orientation.");
    ap_add_optional_arg(display_rotation, "degrees", "0, 90, 180, or 270 (required outside GUI mode)");
    ap_set_helptext(theme, "Select or inspect the UI color theme.");
    ap_set_helptext(theme_list, "List available theme presets.");
    ap_set_helptext(theme_set, "Apply a named theme preset (see 'theme list').");
    ap_add_required_arg(theme_set, "name", "Preset name, e.g. Dracula or \"Dark Gray\"");
    ap_set_helptext(theme_get, "Show the ten active theme colors as RGB565 hex.");
    ap_set_helptext(theme_set_color, "Set one theme color role directly.");
    ap_add_required_arg(
        theme_set_color, "role",
        "primary, secondary, background, surface, text, textMuted, border, success, warning, or error"
    );
    ap_add_required_arg(theme_set_color, "color", "RGB or RRGGBB hex, with or without '#'");

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
    bool theme_hierarchy = root_action == theme;
    bool gui = runtime__gui_requested();
    int result;
    if (clock_hierarchy) {
        result = gui ? config_app__clock_gui()
                     : config_app__clock_cli(clock, show, sync, ntp, timezone, dst, format, set);
    } else if (startup_hierarchy && !gui) {
        result = config_app__startup_cli(startup, startup_add, startup_remove);
    } else if (display_hierarchy) {
        result = gui ? config_app__display_gui()
                     : config_app__display_cli(
                           display, display_buffered, display_brightness, display_dim, display_rotation
                       );
    } else if (theme_hierarchy) {
        ArgParser *theme_action = ap_get_cmd_parser(theme);
        if (theme_action == NULL) result = gui ? config_app__theme_gui() : config_app__theme_show_colors();
        else if (theme_action == theme_list) result = config_app__theme_list_cli();
        else if (theme_action == theme_get) result = config_app__theme_show_colors();
        else if (theme_action == theme_set) result = config_app__theme_set_cli(ap_get_arg(theme_set, "name"));
        else if (theme_action == theme_set_color) {
            result = config_app__theme_set_color_cli(
                ap_get_arg(theme_set_color, "role"), ap_get_arg(theme_set_color, "color")
            );
        } else result = BRUCE_ERR_INVALID_ARGUMENT;
    } else {
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
