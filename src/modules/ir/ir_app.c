#include "ir_app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/input.h"
#include "core_sdk/ir.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#define IR_APP_CAPTURE_SIZE 8192u
#define IR_APP_LIBRARY_PATH "/BruceIR"

typedef struct {
    const char *protocol;
    const char *data;
    uint8_t bits;
} ir_app__power_code_t;

/* A native, protocol-level power database. Duplicates used by many brands are
 * intentionally sent once; regional runs finish with the universal set. */
static const ir_app__power_code_t s_power_na[] = {
    {"NEC",       "20DF10EF", 32},
    {"NEC",       "E0E040BF", 32},
    {"NEC",       "04FB08F7", 32},
    {"NEC",       "40BF12ED", 32},
    {"NEC",       "C1AA09F6", 32},
    {"NEC",       "F50A03FC", 32},
    {"Samsung32", "E0E040BF", 32},
    {"SIRC",      "A90",      12},
    {"SIRC15",    "540C",     15},
};

static const ir_app__power_code_t s_power_eu[] = {
    {"NEC",       "20DF10EF", 32},
    {"NEC",       "E0E040BF", 32},
    {"NEC",       "10EF38C7", 32},
    {"NEC",       "02FD48B7", 32},
    {"NEC",       "08F7C03F", 32},
    {"NEC",       "807F02FD", 32},
    {"Samsung32", "E0E040BF", 32},
    {"SIRC",      "A90",      12},
    {"SIRC20",    "000A90",   20},
};

static const ir_app__power_code_t s_power_universal[] = {
    {"NEC",       "00FF02FD", 32},
    {"NEC",       "00FF38C7", 32},
    {"NEC",       "00FFA25D", 32},
    {"NEC",       "00FFE21D", 32},
    {"NEC",       "FF00FD02", 32},
    {"Samsung32", "707000FF", 32},
};

static const char *const s_tv_buttons[] = {
    "POWER", "UP",   "DOWN",     "LEFT",    "RIGHT", "OK",   "SOURCES", "VOL+",  "VOL-", "CHA+",
    "CHA-",  "MUTE", "SETTINGS", "NETFLIX", "HOME",  "BACK", "EXIT",    "SMART", "1",    "2",
    "3",     "4",    "5",        "6",       "7",     "8",    "9",       "0",
};
static const char *const s_ac_buttons[] = {
    "POWER",
    "TEMP+",
    "TEMP-",
    "SPEED",
    "SWING",
    "SWING+",
    "SWING-",
    "JET",
    "UP",
    "DOWN",
    "MODE",
};
static const char *const s_fan_buttons[] = {
    "POWER",
    "SPEED+",
    "SPEED-",
    "MODE",
    "TIMER",
    "SWING",
    "OSCILLATE",
    "UP",
    "DOWN",
    "LIGHT",
    "ION",
    "SLEEP",
};
static const char *const s_sound_buttons[] = {
    "POWER",    "UP",   "DOWN", "LEFT", "RIGHT",      "OK",   "SOURCES", "VOL+", "VOL-",    "MUTE",
    "SETTINGS", "BACK", "EQ",   "REC",  "PLAY-PAUSE", "STOP", "NEXT",    "PREV", "SHUFFLE", "REPEAT",
};
static const char *const s_led_buttons[] = {
    "ON",       "OFF",    "BRIGHTNESS+", "BRIGHTNESS-", "RED",       "GREEN",
    "BLUE",     "WHITE",  "ORANGE",      "PEA_GREEN",   "DARK_BLUE", "DARK_YELLOW",
    "CYAN",     "PURPLE", "YELLOW",      "LIGHT_BLUE",  "MAGENTA",   "LIGHT_YELLOW",
    "SKY_BLUE", "ROSE",   "MODE_FLASH",  "MODE_STROBE", "MODE_FADE", "MODE_SMOOTH",
};

static bool ir_app__parse_u32(const char *text, uint32_t maximum, uint32_t *out) {
    if (text == NULL || out == NULL) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > maximum) return false;
    *out = (uint32_t)parsed;
    return true;
}

static bruce_result_t ir_app__write_all(bruce_file_id_t file, const char *text, size_t size) {
    size_t total = 0;
    while (total < size) {
        size_t written = 0;
        bruce_result_t result = storage__write(file, text + total, size - total, &written);
        if (result != BRUCE_OK) return result;
        if (written == 0) return BRUCE_ERR_IO;
        total += written;
    }
    return BRUCE_OK;
}

static void ir_app__sanitize_name(const char *input, char *output, size_t output_size) {
    size_t used = 0;
    for (; input != NULL && *input != '\0' && used + 1 < output_size; ++input) {
        unsigned char ch = (unsigned char)*input;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '+' || ch == '.') output[used++] = (char)ch;
        else if (isspace(ch) && used > 0 && output[used - 1] != '_') output[used++] = '_';
    }
    while (used > 0 && (output[used - 1] == '.' || output[used - 1] == '_')) used--;
    output[used] = '\0';
}

static bruce_result_t ir_app__write_capture(bruce_file_id_t file, const char *capture, const char *name) {
    const char *record = strstr(capture, "name:");
    if (record == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    const char *fields = strchr(record, '\n');
    if (fields == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    fields++;
    char clean_name[BRUCE_STORAGE_NAME_MAX];
    ir_app__sanitize_name(name, clean_name, sizeof(clean_name));
    if (clean_name[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;

    char line[BRUCE_STORAGE_NAME_MAX + 8];
    int length = snprintf(line, sizeof(line), "name: %s\n", clean_name);
    bruce_result_t result = ir_app__write_all(file, line, (size_t)length);
    if (result == BRUCE_OK) result = ir_app__write_all(file, fields, strlen(fields));
    return result;
}

static bool ir_app__file_exists(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return false;
    (void)storage__close(file);
    return true;
}

static bruce_result_t ir_app__prepare_learning_file(
    const char *requested_name, char *path, size_t path_size, bruce_file_id_t *out_file
) {
    bruce_result_t result = storage__mkdir(IR_APP_LIBRARY_PATH);
    if (result != BRUCE_OK) return result;

    char entered[BRUCE_STORAGE_NAME_MAX];
    snprintf(entered, sizeof(entered), "%s", requested_name != NULL ? requested_name : "remote");
    for (;;) {
        char clean[BRUCE_STORAGE_NAME_MAX];
        ir_app__sanitize_name(entered, clean, sizeof(clean));
        size_t length = strlen(clean);
        if (length > 3 && strcasecmp(clean + length - 3, ".ir") == 0) clean[length - 3] = '\0';
        if (clean[0] == '\0' ||
            snprintf(path, path_size, "%s/%s.ir", IR_APP_LIBRARY_PATH, clean) >= (int)path_size) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }

        if (ir_app__file_exists(path)) {
            const bruce_dialog_choice_t collision[] = {
                {.label = "Append number", .value = "number"   },
                {.label = "Overwrite",     .value = "overwrite"},
                {.label = "Change name",   .value = "rename"   },
                {.label = "Cancel",        .value = "cancel"   },
            };
            size_t selected = 0;
            result = dialog__choice("Remote exists", path, collision, 4, &selected, NULL);
            if (result != BRUCE_OK || selected == 3) return BRUCE_ERR_CANCELLED;
            if (selected == 0) {
                bool available = false;
                for (unsigned int suffix = 1; suffix < 1000; ++suffix) {
                    if (snprintf(path, path_size, "%s/%s_%u.ir", IR_APP_LIBRARY_PATH, clean, suffix) >=
                        (int)path_size) {
                        return BRUCE_ERR_RESOURCE_LIMIT;
                    }
                    if (!ir_app__file_exists(path)) {
                        available = true;
                        break;
                    }
                }
                if (!available) return BRUCE_ERR_RESOURCE_LIMIT;
            } else if (selected == 2) {
                result =
                    dialog__text_input("Remote name", "Filename", entered, false, entered, sizeof(entered));
                if (result != BRUCE_OK) return result;
                continue;
            }
        }
        break;
    }

    result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, out_file
    );
    if (result != BRUCE_OK) return result;
    static const char header[] = "Filetype: Bruce IR File\nVersion: 1\n#\n";
    result = ir_app__write_all(*out_file, header, sizeof(header) - 1u);
    if (result != BRUCE_OK) (void)storage__close(*out_file);
    return result;
}

static int ir_app__receive(bool raw, uint32_t timeout_ms, bool show_dialog) {
    char *capture = memory__malloc(IR_APP_CAPTURE_SIZE);
    if (capture == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = ir__receive(raw, timeout_ms, capture, IR_APP_CAPTURE_SIZE);
    if (result == BRUCE_OK) {
        if (show_dialog) (void)dialog__message(BRUCE_DIALOG_INFO, "IR capture", capture);
        else stdio__printf("%s", capture);
    } else if (result == BRUCE_ERR_TIMEOUT) stdio__printf("IR receive timed out\n");
    else if (result == BRUCE_ERR_UNSUPPORTED) stdio__printf("IR decoding failed; retry with raw mode\n");
    else stdio__printf("IR receive failed: %d\n", result);
    memory__free(capture);
    return result;
}

static bruce_result_t ir_app__capture_signal(bool raw, const char *button, char *capture) {
    char message[128];
    snprintf(message, sizeof(message), "Point the remote at RX GPIO %d and press %s", ir__rx_pin(), button);
    (void)dialog__message(BRUCE_DIALOG_INFO, "Learn signal", message);
    return ir__receive(raw, 10000, capture, IR_APP_CAPTURE_SIZE);
}

static bruce_result_t ir_app__custom_learn(void) {
    const bruce_dialog_choice_t modes[] = {
        {.label = "Decoded signal", .value = "decoded"},
        {.label = "Raw signal",     .value = "raw"    },
        {.label = "Cancel",         .value = "cancel" },
    };
    size_t mode = 0;
    bruce_result_t result = dialog__choice("Custom learn", "Capture format", modes, 3, &mode, NULL);
    if (result != BRUCE_OK || mode == 2) return BRUCE_ERR_CANCELLED;

    char *capture = memory__malloc(IR_APP_CAPTURE_SIZE);
    if (capture == NULL) return BRUCE_ERR_NO_MEMORY;
    result = ir_app__capture_signal(mode == 1, "the button", capture);
    if (result == BRUCE_ERR_UNSUPPORTED && mode == 0) {
        const bruce_dialog_choice_t retry[] = {
            {.label = "Retry as raw", .value = "raw"   },
            {.label = "Cancel",       .value = "cancel"},
        };
        size_t selected = 0;
        if (dialog__choice(
                "Unknown protocol", "Capture this signal as raw timings?", retry, 2, &selected, NULL
            ) == BRUCE_OK &&
            selected == 0) {
            result = ir_app__capture_signal(true, "the button again", capture);
        }
    }
    if (result != BRUCE_OK) {
        memory__free(capture);
        return result;
    }

    for (;;) {
        const bruce_dialog_choice_t actions[] = {
            {.label = "Save",        .value = "save"   },
            {.label = "Test signal", .value = "test"   },
            {.label = "View",        .value = "view"   },
            {.label = "Discard",     .value = "discard"},
        };
        size_t action = 0;
        result = dialog__choice("Signal captured", "Choose an action", actions, 4, &action, NULL);
        if (result != BRUCE_OK || action == 3) break;
        if (action == 1) {
            result = ir__transmit_record(capture, 0);
            (void)dialog__message(
                result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_ERROR,
                "Test signal",
                result == BRUCE_OK ? "Signal sent" : "Transmission failed"
            );
            continue;
        }
        if (action == 2) {
            (void)dialog__message(BRUCE_DIALOG_INFO, "Captured record", capture);
            continue;
        }

        char button[BRUCE_STORAGE_NAME_MAX] = "POWER";
        char remote[BRUCE_STORAGE_NAME_MAX] = "remote";
        result = dialog__text_input("Button name", "Signal name", button, false, button, sizeof(button));
        if (result == BRUCE_OK)
            result = dialog__text_input(
                "Remote name", "Saved under /BruceIR", remote, false, remote, sizeof(remote)
            );
        char path[BRUCE_STORAGE_PATH_MAX];
        bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
        if (result == BRUCE_OK) result = ir_app__prepare_learning_file(remote, path, sizeof(path), &file);
        if (result == BRUCE_OK) result = ir_app__write_capture(file, capture, button);
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)dialog__message(
            result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_ERROR,
            "Learn remote",
            result == BRUCE_OK ? path : "Could not save signal"
        );
        break;
    }
    memory__free(capture);
    return result == BRUCE_ERR_CANCELLED ? BRUCE_OK : result;
}

static bruce_result_t ir_app__quick_learn(void) {
    const bruce_dialog_choice_t devices[] = {
        {.label = "TV",        .value = "tv"    },
        {.label = "AC",        .value = "ac"    },
        {.label = "Fan",       .value = "fan"   },
        {.label = "Sound",     .value = "sound" },
        {.label = "LED strip", .value = "led"   },
        {.label = "Cancel",    .value = "cancel"},
    };
    size_t device = 0;
    bruce_result_t result =
        dialog__choice("Quick remote setup", "Device template", devices, 6, &device, NULL);
    if (result != BRUCE_OK || device == 5) return BRUCE_ERR_CANCELLED;

    const char *const *buttons = s_tv_buttons;
    size_t button_count = sizeof(s_tv_buttons) / sizeof(s_tv_buttons[0]);
    if (device == 1) {
        buttons = s_ac_buttons;
        button_count = sizeof(s_ac_buttons) / sizeof(s_ac_buttons[0]);
    } else if (device == 2) {
        buttons = s_fan_buttons;
        button_count = sizeof(s_fan_buttons) / sizeof(s_fan_buttons[0]);
    } else if (device == 3) {
        buttons = s_sound_buttons;
        button_count = sizeof(s_sound_buttons) / sizeof(s_sound_buttons[0]);
    } else if (device == 4) {
        buttons = s_led_buttons;
        button_count = sizeof(s_led_buttons) / sizeof(s_led_buttons[0]);
    }

    const bruce_dialog_choice_t formats[] = {
        {.label = "Raw (recommended)", .value = "raw"    },
        {.label = "Decoded NEC",       .value = "decoded"},
    };
    size_t format = 0;
    result = dialog__choice("Quick remote setup", "Capture format", formats, 2, &format, NULL);
    if (result != BRUCE_OK) return result;

    char remote[BRUCE_STORAGE_NAME_MAX];
    snprintf(remote, sizeof(remote), "%s_remote", devices[device].label);
    result = dialog__text_input("Remote name", "Saved under /BruceIR", remote, false, remote, sizeof(remote));
    char path[BRUCE_STORAGE_PATH_MAX] = {0};
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;

    char *capture = memory__malloc(IR_APP_CAPTURE_SIZE);
    if (capture == NULL) { return BRUCE_ERR_NO_MEMORY; }
    size_t saved = 0;
    for (size_t button = 0; button < button_count;) {
        result = ir_app__capture_signal(format == 0, buttons[button], capture);
        if (result != BRUCE_OK) {
            const bruce_dialog_choice_t failed[] = {
                {.label = "Retry",  .value = "retry" },
                {.label = "Skip",   .value = "skip"  },
                {.label = "Finish", .value = "finish"},
            };
            size_t action = 0;
            if (dialog__choice("No signal", buttons[button], failed, 3, &action, NULL) != BRUCE_OK ||
                action == 2)
                break;
            if (action == 1) button++;
            continue;
        }

        const bruce_dialog_choice_t captured[] = {
            {.label = "Save",   .value = "save"  },
            {.label = "Test",   .value = "test"  },
            {.label = "Retry",  .value = "retry" },
            {.label = "Skip",   .value = "skip"  },
            {.label = "Finish", .value = "finish"},
        };
        size_t action = 0;
        if (dialog__choice("Captured", buttons[button], captured, 5, &action, NULL) != BRUCE_OK ||
            action == 4)
            break;
        if (action == 1) {
            (void)ir__transmit_record(capture, 0);
            continue;
        }
        if (action == 2) continue;
        if (action == 3) {
            button++;
            continue;
        }
        if (file == BRUCE_FILE_ID_INVALID) {
            result = ir_app__prepare_learning_file(remote, path, sizeof(path), &file);
            if (result != BRUCE_OK) break;
        }
        result = ir_app__write_capture(file, capture, buttons[button]);
        if (result != BRUCE_OK) break;
        saved++;
        button++;
    }
    memory__free(capture);
    if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);

    char summary[BRUCE_STORAGE_PATH_MAX + 40];
    if (saved > 0) snprintf(summary, sizeof(summary), "Saved %u signal(s) to %s", (unsigned int)saved, path);
    else snprintf(summary, sizeof(summary), "No signals saved");
    (void)dialog__message(
        result == BRUCE_OK && saved > 0 ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_WARNING,
        "Quick remote setup",
        summary
    );
    return result != BRUCE_OK ? result : (saved > 0 ? BRUCE_OK : BRUCE_ERR_CANCELLED);
}

static bruce_result_t ir_app__send_power_batch(
    const ir_app__power_code_t *codes, size_t count, size_t *sent, size_t total, bruce_viewer_id_t viewer
) {
    for (size_t i = 0; i < count; ++i) {
        if (input__check(BRUCE_INPUT_CODE_BACK, true) || input__check(BRUCE_INPUT_CODE_BUTTON_B, true)) {
            return BRUCE_ERR_CANCELLED;
        }
        bruce_result_t result = ir__transmit(codes[i].data, codes[i].protocol, codes[i].bits, 0);
        if (result != BRUCE_OK) return result;
        (*sent)++;
        if (viewer != BRUCE_VIEWER_ID_INVALID) {
            char progress[96];
            snprintf(
                progress,
                sizeof(progress),
                "Sending power codes\n%u / %u\n\nBack: stop",
                (unsigned int)*sent,
                (unsigned int)total
            );
            (void)dialog__viewer_set_text(viewer, progress);
        }
        if (runtime__delay(205) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    return BRUCE_OK;
}

static bruce_result_t ir_app__tvbgone(bool europe, bool gui) {
    const ir_app__power_code_t *regional = europe ? s_power_eu : s_power_na;
    size_t regional_count =
        europe ? sizeof(s_power_eu) / sizeof(s_power_eu[0]) : sizeof(s_power_na) / sizeof(s_power_na[0]);
    size_t universal_count = sizeof(s_power_universal) / sizeof(s_power_universal[0]);
    size_t sent = 0;
    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    if (gui) {
        (void)input__flush();
        (void)dialog__create_text_viewer("TV-B-Gone", "Starting...\n\nBack: stop", &viewer);
    }
    bruce_result_t result =
        ir_app__send_power_batch(regional, regional_count, &sent, regional_count + universal_count, viewer);
    if (result == BRUCE_OK)
        result = ir_app__send_power_batch(
            s_power_universal, universal_count, &sent, regional_count + universal_count, viewer
        );
    if (viewer != BRUCE_VIEWER_ID_INVALID) (void)dialog__viewer_close(viewer);
    if (gui)
        (void)dialog__message(
            result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_WARNING,
            "TV-B-Gone",
            result == BRUCE_OK ? "All codes sent" : "Stopped"
        );
    return result;
}

static void
ir_app__jam_pattern(unsigned int mode, uint32_t *timings, size_t count, uint32_t *state, uint32_t *sweep) {
    for (size_t i = 0; i < count; ++i) {
        if (mode == 0) timings[i] = 12;
        else if (mode == 1) timings[i] = (i & 1u) == 0 ? 12 : 24;
        else if (mode == 2) timings[i] = *sweep;
        else if (mode == 3) {
            *state = *state * 1664525u + 1013904223u;
            timings[i] = 5u + (*state % 995u);
        } else timings[i] = 1;
    }
    if (mode == 2) {
        *sweep += 3;
        if (*sweep > 70) *sweep = 8;
    }
}

static bruce_result_t ir_app__jammer(uint32_t frequency, uint32_t duration_ms, unsigned int mode, bool gui) {
    static const uint32_t frequencies[] = {30000, 33000, 36000, 38000, 40000, 42000, 56000};
    size_t frequency_index = 3;
    bool listed_frequency = false;
    for (size_t i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); ++i) {
        if (frequencies[i] == frequency) {
            frequency_index = i;
            listed_frequency = true;
        }
    }
    uint32_t timings[BRUCE_IR_MAX_RAW_TIMINGS];
    uint32_t random_state = 0x42525543u;
    uint32_t sweep = 8;
    uint32_t elapsed = 0;
    bool active = true;
    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    if (gui) {
        (void)input__flush();
        (void)dialog__create_text_viewer("IR jammer", "Starting...", &viewer);
    }
    while (duration_ms == 0 || elapsed < duration_ms) {
        bruce_input_event_t event;
        if (gui && input__poll(&event) == BRUCE_OK && event.action == BRUCE_INPUT_PRESS) {
            if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B) break;
            if (event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A)
                active = !active;
            if (event.code == BRUCE_INPUT_CODE_LEFT && frequency_index > 0) frequency_index--;
            if (event.code == BRUCE_INPUT_CODE_RIGHT &&
                frequency_index + 1 < sizeof(frequencies) / sizeof(frequencies[0])) {
                frequency_index++;
            }
        }
        if (active) {
            size_t count = mode == 3 ? 64 : BRUCE_IR_MAX_RAW_TIMINGS;
            ir_app__jam_pattern(mode, timings, count, &random_state, &sweep);
            uint32_t active_frequency = gui || listed_frequency ? frequencies[frequency_index] : frequency;
            bruce_result_t result = ir__transmit_raw(timings, count, active_frequency, 0);
            if (result != BRUCE_OK) {
                if (viewer != BRUCE_VIEWER_ID_INVALID) (void)dialog__viewer_close(viewer);
                return result;
            }
        }
        if (viewer != BRUCE_VIEWER_ID_INVALID) {
            static const char *const names[] = {"Basic", "Enhanced", "Sweep", "Random", "Empty"};
            char status[160];
            snprintf(
                status,
                sizeof(status),
                "%s\n%s\n%u Hz\n\nLeft/Right: carrier\nSelect: pause\nBack: stop",
                active ? "ACTIVE" : "PAUSED",
                names[mode],
                (unsigned int)frequencies[frequency_index]
            );
            (void)dialog__viewer_set_text(viewer, status);
        }
        if (runtime__delay(2) != BRUCE_OK) break;
        elapsed += 2;
    }
    if (viewer != BRUCE_VIEWER_ID_INVALID) (void)dialog__viewer_close(viewer);
    return BRUCE_OK;
}

static int ir_app__gui(void) {
    char message[80];
    snprintf(message, sizeof(message), "TX GPIO %d, RX GPIO %d", ir__tx_pin(), ir__rx_pin());
    const bruce_dialog_choice_t choices[] = {
        {.label = "TV-B-Gone",          .value = "tvbgone"},
        {.label = "IR jammer",          .value = "jammer" },
        {.label = "Learn signal",       .value = "learn"  },
        {.label = "Quick remote setup", .value = "quick"  },
        {.label = "Transmit .ir file",  .value = "file"   },
        {.label = "Read/view signal",   .value = "read"   },
        {.label = "Exit",               .value = "exit"   },
    };
    for (;;) {
        size_t selected = 0;
        bruce_result_t result = dialog__choice("Infrared", message, choices, 7, &selected, NULL);
        if (result == BRUCE_ERR_CANCELLED || selected == 6) return 0;
        if (result != BRUCE_OK) return result;
        if (selected == 0) {
            const bruce_dialog_choice_t regions[] = {
                {.label = "North America / Asia", .value = "na"    },
                {.label = "Europe / other",       .value = "eu"    },
                {.label = "Cancel",               .value = "cancel"},
            };
            size_t region = 0;
            if (dialog__choice("TV-B-Gone", "Select region", regions, 3, &region, NULL) == BRUCE_OK &&
                region < 2) {
                (void)ir_app__tvbgone(region == 1, true);
            }
        } else if (selected == 1) {
            const bruce_dialog_choice_t modes[] = {
                {.label = "Basic",          .value = "basic"   },
                {.label = "Enhanced basic", .value = "enhanced"},
                {.label = "Sweep",          .value = "sweep"   },
                {.label = "Random",         .value = "random"  },
                {.label = "Empty",          .value = "empty"   },
                {.label = "Cancel",         .value = "cancel"  },
            };
            size_t mode = 0;
            if (dialog__choice("IR jammer", "Select pattern", modes, 6, &mode, NULL) == BRUCE_OK &&
                mode < 5) {
                (void)ir_app__jammer(38000, 0, (unsigned int)mode, true);
            }
        } else if (selected == 2) (void)ir_app__custom_learn();
        else if (selected == 3) (void)ir_app__quick_learn();
        else if (selected == 4) {
            char path[BRUCE_STORAGE_PATH_MAX];
            result = dialog__pick_file("/", ".ir", path, sizeof(path));
            if (result == BRUCE_OK) result = ir__transmit_file(path, 0);
            if (result != BRUCE_ERR_CANCELLED) {
                (void)dialog__message(
                    result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_ERROR,
                    "Infrared",
                    result == BRUCE_OK ? "Transmission complete" : "Transmission failed"
                );
            }
        } else if (selected == 5) {
            const bruce_dialog_choice_t read_modes[] = {
                {.label = "Decoded signal", .value = "decoded"},
                {.label = "Raw signal",     .value = "raw"    },
                {.label = "Cancel",         .value = "cancel" },
            };
            size_t read_mode = 0;
            if (dialog__choice("Read signal", "Capture format", read_modes, 3, &read_mode, NULL) ==
                    BRUCE_OK &&
                read_mode < 2) {
                (void)ir_app__receive(read_mode == 1, 10000, true);
            }
        }
    }
}

static int ir_app__rx(ArgParser *parser) {
    bool raw = false;
    uint32_t timeout_seconds = 10;
    int index = 0;
    if (ap_count_args(parser) > index && strcmp(ap_get_arg_at_index(parser, index), "raw") == 0) {
        raw = true;
        index++;
    }
    if (ap_count_args(parser) > index) {
        if (!ir_app__parse_u32(ap_get_arg_at_index(parser, index), UINT32_MAX / 1000u, &timeout_seconds) ||
            timeout_seconds == 0) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    }
    stdio__printf("Waiting for IR signal...\n");
    return ir_app__receive(raw, timeout_seconds * 1000u, false);
}

static int ir_app__tx(ArgParser *parser) {
    const char *protocol = ap_get_arg(parser, "protocol");
    const char *value = ap_get_arg(parser, "data_or_address");
    const char *third = ap_get_arg(parser, "bits_or_command");
    const char *fourth = ap_get_arg(parser, "repeats");
    if (protocol == NULL || value == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (third != NULL && fourth == NULL && strlen(value) == 8 && strlen(third) == 8) {
        return ir__transmit_parsed(protocol, value, third, 0);
    }
    uint32_t bits = 32;
    uint32_t repeats = 0;
    if ((third != NULL && !ir_app__parse_u32(third, 32, &bits)) || bits == 0 ||
        (fourth != NULL && !ir_app__parse_u32(fourth, UINT8_MAX, &repeats)))
        return BRUCE_ERR_INVALID_ARGUMENT;
    return ir__transmit(value, protocol, (uint8_t)bits, (uint8_t)repeats);
}

static int ir_app__tx_raw(ArgParser *parser) {
    int argc = ap_count_args(parser);
    if (argc < 2) return BRUCE_ERR_INVALID_ARGUMENT;
    uint32_t frequency = 0;
    if (!ir_app__parse_u32(ap_get_arg_at_index(parser, 0), 100000, &frequency)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    uint32_t *timings = memory__malloc(BRUCE_IR_MAX_RAW_TIMINGS * sizeof(*timings));
    if (timings == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t count = 0;
    for (int arg = 1; arg < argc; ++arg) {
        const char *cursor = ap_get_arg_at_index(parser, arg);
        while (*cursor != '\0') {
            while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
            if (*cursor == '\0') break;
            char *end = NULL;
            unsigned long duration = strtoul(cursor, &end, 10);
            if (end == cursor || duration == 0 || duration > 32767 || count >= BRUCE_IR_MAX_RAW_TIMINGS) {
                memory__free(timings);
                return count >= BRUCE_IR_MAX_RAW_TIMINGS ? BRUCE_ERR_RESOURCE_LIMIT
                                                         : BRUCE_ERR_INVALID_ARGUMENT;
            }
            timings[count++] = (uint32_t)duration;
            cursor = end;
        }
    }
    bruce_result_t result = ir__transmit_raw(timings, count, frequency, 0);
    memory__free(timings);
    return result;
}

static int ir_app__learn_cli(ArgParser *parser) {
    const char *path = ap_get_arg(parser, "absolute_path");
    const char *button = ap_get_arg(parser, "button_name");
    if (path == NULL || button == NULL || path[0] != '/') return BRUCE_ERR_INVALID_ARGUMENT;
    const char *format_or_timeout = ap_get_arg(parser, "raw_or_timeout");
    bool raw = format_or_timeout != NULL && strcmp(format_or_timeout, "raw") == 0;
    const char *timeout_arg = raw ? ap_get_arg(parser, "timeout_seconds") : format_or_timeout;
    uint32_t timeout = 10;
    if (timeout_arg != NULL && !ir_app__parse_u32(timeout_arg, UINT32_MAX / 1000u, &timeout)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    char *capture = memory__malloc(IR_APP_CAPTURE_SIZE);
    if (capture == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = ir__receive(raw, timeout * 1000u, capture, IR_APP_CAPTURE_SIZE);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (result == BRUCE_OK) {
        result = storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        );
    }
    static const char header[] = "Filetype: Bruce IR File\nVersion: 1\n#\n";
    if (result == BRUCE_OK) result = ir_app__write_all(file, header, sizeof(header) - 1u);
    if (result == BRUCE_OK) result = ir_app__write_capture(file, capture, button);
    if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
    memory__free(capture);
    return result;
}

int ir_app_main(int argc, char **argv) {
    if (app_runner__args_have_gui(argc, argv)) {
        if (!app_runner__args_have_background(argc, argv)) {
            bruce_result_t foreground = process__to_foreground();
            if (foreground != BRUCE_OK) return foreground;
        }
        return ir_app__gui();
    }

    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Receive, learn, transmit, and generate infrared signals.");
    ap_add_flag(root, "gui");
    ap_set_opt_help(root, "gui", "Use GUI interaction mode");

    ArgParser *rx = ap_new_cmd(root, "rx");
    ArgParser *learn = ap_new_cmd(root, "learn");
    ArgParser *tx = ap_new_cmd(root, "tx");
    ArgParser *tx_raw = ap_new_cmd(root, "tx_raw");
    ArgParser *tx_from_file = ap_new_cmd(root, "tx_from_file");
    ArgParser *tvbgone = ap_new_cmd(root, "tvbgone");
    ArgParser *jam = ap_new_cmd(root, "jam");
    ArgParser *commands[] = {rx, learn, tx, tx_raw, tx_from_file, tvbgone, jam};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (commands[i] == NULL) {
            ap_free(root);
            return BRUCE_ERR_NO_MEMORY;
        }
    }

    ap_set_helptext(rx, "Receive a decoded or raw signal: rx [raw] [timeout_seconds].");
    ap_add_optional_arg(rx, "raw_or_timeout", "The word raw, or timeout in seconds");
    ap_add_optional_arg(rx, "timeout_seconds", "Timeout when raw is specified");
    ap_set_helptext(learn, "Capture one signal and write a Bruce IR file.");
    ap_add_required_arg(learn, "absolute_path", "Destination .ir file path");
    ap_add_required_arg(learn, "button_name", "Name stored for the signal");
    ap_add_optional_arg(learn, "raw_or_timeout", "The word raw, or timeout in seconds");
    ap_add_optional_arg(learn, "timeout_seconds", "Timeout when raw is specified");
    ap_set_helptext(tx, "Transmit protocol data, or an address/command pair for supported protocols.");
    ap_add_required_arg(tx, "protocol", "IR protocol name");
    ap_add_required_arg(tx, "data_or_address", "Hex data or protocol address");
    ap_add_optional_arg(tx, "bits_or_command", "Bit count or protocol command");
    ap_add_optional_arg(tx, "repeats", "Repeat count (0-255)");
    ap_set_helptext(tx_raw, "Transmit raw timings: tx_raw <frequency_hz> <timing_us> [...]. Commas are accepted.");
    ap_add_required_arg(tx_raw, "frequency_hz", "Carrier frequency in Hz");
    ap_allow_extra_args(tx_raw);
    ap_first_pos_arg_ends_option_parsing(tx_raw);
    ap_set_helptext(tx_from_file, "Transmit every signal in a Bruce IR file.");
    ap_add_required_arg(tx_from_file, "absolute_path", "Source .ir file path");
    ap_add_optional_arg(tx_from_file, "repeats", "Repeat count (0-255)");
    ap_set_helptext(tvbgone, "Send regional TV power codes.");
    ap_add_required_arg(tvbgone, "region", "na or eu (case-insensitive)");
    ap_set_helptext(jam, "Transmit an IR jamming pattern.");
    ap_add_required_arg(jam, "frequency_hz", "Carrier frequency from 20000 to 100000 Hz");
    ap_add_required_arg(jam, "seconds", "Duration in seconds");
    ap_add_optional_arg(jam, "mode", "basic, enhanced, sweep, random, or empty");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return 0;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    ArgParser *command = ap_get_cmd_parser(root);
    bool is_rx = command == rx;
    if (command == NULL) {
        ap_print_help(root);
        result = BRUCE_OK;
    } else if (command == rx) result = ir_app__rx(rx);
    else if (command == learn) result = ir_app__learn_cli(learn);
    else if (command == tx) result = ir_app__tx(tx);
    else if (command == tx_raw) result = ir_app__tx_raw(tx_raw);
    else if (command == tx_from_file) {
        uint32_t repeats = 0;
        const char *repeats_arg = ap_get_arg(tx_from_file, "repeats");
        if (repeats_arg != NULL && !ir_app__parse_u32(repeats_arg, UINT8_MAX, &repeats)) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else result = ir__transmit_file(ap_get_arg(tx_from_file, "absolute_path"), (uint8_t)repeats);
    } else if (command == tvbgone) {
        const char *region = ap_get_arg(tvbgone, "region");
        if (strcasecmp(region, "na") != 0 && strcasecmp(region, "eu") != 0) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else result = ir_app__tvbgone(strcasecmp(region, "eu") == 0, false);
    } else if (command == jam) {
        uint32_t frequency = 0;
        uint32_t seconds = 0;
        unsigned int mode = 0;
        const char *frequency_arg = ap_get_arg(jam, "frequency_hz");
        const char *seconds_arg = ap_get_arg(jam, "seconds");
        const char *mode_arg = ap_get_arg(jam, "mode");
        if (!ir_app__parse_u32(frequency_arg, 100000, &frequency) || frequency < 20000 ||
            !ir_app__parse_u32(seconds_arg, UINT32_MAX / 1000u, &seconds) || seconds == 0) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            static const char *const modes[] = {"basic", "enhanced", "sweep", "random", "empty"};
            if (mode_arg != NULL) {
                for (mode = 0; mode < 5 && strcasecmp(mode_arg, modes[mode]) != 0; ++mode) {}
            }
            result = mode == 5 ? BRUCE_ERR_INVALID_ARGUMENT
                               : ir_app__jammer(frequency, seconds * 1000u, mode, false);
        }
    }
    ap_free(root);
    if (!is_rx && command != NULL)
        stdio__printf(result == BRUCE_OK ? "IR operation complete\n" : "IR operation failed: %d\n", result);
    return result;
}
