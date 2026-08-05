#include "input_keyboard.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "input_common.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/gpio.h"
#include "core_sdk/process.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

#define TAG "bruce_input_kb"

#define INPUT__HAS_KEYBOARD CONFIG_BRUCE_KEYBOARD_ENABLED
#if CONFIG_BRUCE_KEYBOARD_ENABLED
#define INPUT__KB_OUT_PINS                                                                                   \
    {CONFIG_BRUCE_KEYBOARD_OUT0_GPIO, CONFIG_BRUCE_KEYBOARD_OUT1_GPIO, CONFIG_BRUCE_KEYBOARD_OUT2_GPIO}
#define INPUT__KB_IN_PINS                                                                                   \
    {CONFIG_BRUCE_KEYBOARD_IN0_GPIO, CONFIG_BRUCE_KEYBOARD_IN1_GPIO, CONFIG_BRUCE_KEYBOARD_IN2_GPIO,         \
     CONFIG_BRUCE_KEYBOARD_IN3_GPIO, CONFIG_BRUCE_KEYBOARD_IN4_GPIO, CONFIG_BRUCE_KEYBOARD_IN5_GPIO,         \
     CONFIG_BRUCE_KEYBOARD_IN6_GPIO}
#define INPUT__KB_AUTODETECT_ALT CONFIG_BRUCE_KEYBOARD_AUTODETECT_ALT
#if CONFIG_BRUCE_KEYBOARD_AUTODETECT_ALT
#define INPUT__KB_IN_ALT_PINS                                                                                \
    {CONFIG_BRUCE_KEYBOARD_IN_ALT0_GPIO, CONFIG_BRUCE_KEYBOARD_IN_ALT1_GPIO,                                 \
     CONFIG_BRUCE_KEYBOARD_IN_ALT2_GPIO, CONFIG_BRUCE_KEYBOARD_IN_ALT3_GPIO,                                 \
     CONFIG_BRUCE_KEYBOARD_IN_ALT4_GPIO, CONFIG_BRUCE_KEYBOARD_IN_ALT5_GPIO,                                 \
     CONFIG_BRUCE_KEYBOARD_IN_ALT6_GPIO}
#endif
#endif

#ifndef INPUT__KB_SCAN_SETTLE_US
#ifdef CONFIG_BRUCE_INPUT_SCAN_SETTLE_US
#define INPUT__KB_SCAN_SETTLE_US CONFIG_BRUCE_INPUT_SCAN_SETTLE_US
#else
#define INPUT__KB_SCAN_SETTLE_US 30
#endif
#endif

#if INPUT__HAS_KEYBOARD

#define INPUT__KB_OUT_COUNT 3
#define INPUT__KB_IN_COUNT 7
#define INPUT__KB_COLS 14
#define INPUT__KB_ROWS 4

/* Visual layout (row 0 is the top `123... row). */
static const char *const s_kb_normal[INPUT__KB_ROWS][INPUT__KB_COLS] = {
    {"`",    "1",     "2",   "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "del"  },
    {"tab",  "q",     "w",   "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\"   },
    {"fn",   "shift", "a",   "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "enter"},
    {"ctrl", "opt",   "alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "space"},
};

static const char *const s_kb_shifted[INPUT__KB_ROWS][INPUT__KB_COLS] = {
    {"~",    "!",     "@",   "#", "$", "%", "^", "&", "*", "(", ")", "_", "+",  "del"  },
    {"tab",  "Q",     "W",   "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}",  "|"    },
    {"fn",   "shift", "A",   "S", "D", "F", "G", "H", "J", "K", "L", ":", "\"", "enter"},
    {"ctrl", "opt",   "alt", "Z", "X", "C", "V", "B", "N", "M", "<", ">", "?",  "space"},
};

static const int s_kb_out_pins[INPUT__KB_OUT_COUNT] = INPUT__KB_OUT_PINS;
static const int s_kb_in_pins[INPUT__KB_IN_COUNT] = INPUT__KB_IN_PINS;
#if INPUT__KB_AUTODETECT_ALT
static const int s_kb_in_alt_pins[INPUT__KB_IN_COUNT] = INPUT__KB_IN_ALT_PINS;
#endif

static bool s_kb_use_alt_in_pins;
static bool s_kb_prev_pressed[INPUT__KB_ROWS][INPUT__KB_COLS];
static bool s_kb_hotkey_consumed[INPUT__KB_ROWS][INPUT__KB_COLS];
static uint64_t s_kb_last_event_ms[INPUT__KB_ROWS][INPUT__KB_COLS];
static bool s_kb_fn_held;
static bool s_kb_shift_held;
static bool s_kb_ctrl_held;
static bool s_kb_alt_held;

static bool s_kb_initialized;

static void input__kb_set_output(uint8_t scan_state) {
    scan_state &= 0x07;
    (void)gpio__write(s_kb_out_pins[0], (scan_state & 0x01) ? 1 : 0);
    (void)gpio__write(s_kb_out_pins[1], (scan_state & 0x02) ? 1 : 0);
    (void)gpio__write(s_kb_out_pins[2], (scan_state & 0x04) ? 1 : 0);
}

static uint8_t input__kb_read_inputs(const int *pins) {
    uint8_t mask = 0;
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) {
        int level = 1;
        (void)gpio__read(pins[i], &level);
        if (level == 0) { mask |= (1U << i); }
    }
    return mask;
}

static bool input__kb_scan(bool out_pressed[INPUT__KB_ROWS][INPUT__KB_COLS]) {
    bool any = false;
    memset(out_pressed, 0, sizeof(bool) * INPUT__KB_ROWS * INPUT__KB_COLS);

    for (int scan_state = 0; scan_state < 8; ++scan_state) {
        input__kb_set_output(scan_state);
        esp_rom_delay_us(INPUT__KB_SCAN_SETTLE_US);

        uint8_t in_mask = input__kb_read_inputs(s_kb_use_alt_in_pins ? s_kb_in_alt_pins : s_kb_in_pins);

#if INPUT__KB_AUTODETECT_ALT
        /* If primary pins show nothing, try the alternate IN0/IN1 pins. */
        if (in_mask == 0 && !s_kb_use_alt_in_pins) {
            uint8_t alt_mask = input__kb_read_inputs(s_kb_in_alt_pins);
            if (alt_mask != 0) {
                s_kb_use_alt_in_pins = true;
                ESP_LOGW(TAG, "keyboard switched to alternate IN0/IN1 pins");
                in_mask = alt_mask;
            }
        }
#endif

        if (in_mask == 0) { continue; }

        for (int j = 0; j < INPUT__KB_IN_COUNT; ++j) {
            if ((in_mask & (1U << j)) == 0) { continue; }
            int x = (scan_state > 3) ? (2 * j) : (2 * j + 1);
            int y_base = (scan_state > 3) ? (scan_state - 4) : scan_state;
            int y = 3 - y_base;
            if (x >= 0 && x < INPUT__KB_COLS && y >= 0 && y < INPUT__KB_ROWS) {
                out_pressed[y][x] = true;
                any = true;
            }
        }
    }

    input__kb_set_output(0);
    return any;
}

static int32_t input__kb_char_code(const char *label) {
    if (label == NULL) { return 0; }
    if (strcmp(label, "space") == 0) { return ' '; }
    if (strcmp(label, "enter") == 0) { return '\n'; }
    if (strcmp(label, "tab") == 0) { return '\t'; }
    if (strcmp(label, "del") == 0) { return '\b'; }
    if (label[0] != '\0' && label[1] == '\0') { return (int32_t)(unsigned char)label[0]; }
    return 0;
}

static bool input__kb_is_modifier(int x, int y) {
    const char *label = s_kb_normal[y][x];
    return strcmp(label, "fn") == 0 || strcmp(label, "shift") == 0 || strcmp(label, "ctrl") == 0 ||
           strcmp(label, "alt") == 0;
}

static void input__kb_update_modifiers(const bool pressed[INPUT__KB_ROWS][INPUT__KB_COLS]) {
    s_kb_fn_held = false;
    s_kb_shift_held = false;
    s_kb_ctrl_held = false;
    s_kb_alt_held = false;

    for (int y = 0; y < INPUT__KB_ROWS; ++y) {
        for (int x = 0; x < INPUT__KB_COLS; ++x) {
            if (!pressed[y][x]) { continue; }

            const char *label = s_kb_normal[y][x];
            if (strcmp(label, "fn") == 0) {
                s_kb_fn_held = true;
            } else if (strcmp(label, "shift") == 0) {
                s_kb_shift_held = true;
            } else if (strcmp(label, "ctrl") == 0) {
                s_kb_ctrl_held = true;
            } else if (strcmp(label, "alt") == 0) {
                s_kb_alt_held = true;
            }
        }
    }
}

/* Decode an Fn+key chord into a semantic navigation code.  Returns 0 if none. */
static int32_t input__kb_decode_fn_nav(int x, int y) {
    /* Cardputer captured bindings:
     *   NavUp    : Fn + ;   (row 2, col 11)
     *   NavDown  : Fn + .   (row 3, col 11)
     *   NavLeft  : Fn + ,   (row 3, col 10)
     *   NavRight : Fn + /   (row 3, col 12)
     *   Back/Esc : Fn + 1   (row 0, col 1)
     *   Delete   : Fn + del (row 0, col 13)
     */
    if (y == 2 && x == 11) { return BRUCE_INPUT_CODE_UP; }
    if (y == 3 && x == 11) { return BRUCE_INPUT_CODE_DOWN; }
    if (y == 3 && x == 10) { return BRUCE_INPUT_CODE_LEFT; }
    if (y == 3 && x == 12) { return BRUCE_INPUT_CODE_RIGHT; }
    if (y == 0 && x == 1) { return BRUCE_INPUT_CODE_BACK; }
    if (y == 0 && x == 13) { return BRUCE_INPUT_CODE_DELETE; }
    return 0;
}

static bool input__kb_match_hotkey(const char *key, char *out_action, size_t action_size) {
    char chord[BRUCE_CONFIG_HOTKEY_MAX_LEN + 1] = {0};
    size_t used = 0;
#define INPUT__APPEND_CHORD_PART(part)                                                                       \
    do {                                                                                                     \
        int written = snprintf(chord + used, sizeof(chord) - used, "%s%s", used == 0 ? "" : " + ", part);    \
        if (written < 0 || (size_t)written >= sizeof(chord) - used) return false;                            \
        used += (size_t)written;                                                                             \
    } while (0)
    if (s_kb_fn_held) INPUT__APPEND_CHORD_PART("fn");
    if (s_kb_ctrl_held) INPUT__APPEND_CHORD_PART("ctrl");
    if (s_kb_alt_held) INPUT__APPEND_CHORD_PART("alt");
    if (s_kb_shift_held) INPUT__APPEND_CHORD_PART("shift");
    INPUT__APPEND_CHORD_PART(key);
#undef INPUT__APPEND_CHORD_PART

    const bruce_config_hotkeys_t *hotkeys = config__get_hotkeys();
    if (hotkeys == NULL) return false;
    for (size_t i = 0; i < hotkeys->count; ++i) {
        if (strcmp(hotkeys->items[i].key, chord) != 0) continue;
        int written = snprintf(out_action, action_size, "%s", hotkeys->items[i].action);
        return written > 0 && (size_t)written < action_size;
    }
    return false;
}

/* True iff `command`'s leading "key=value" environment tokens (the same ones
 * app_runner__run_command() parses off the front of the line) include
 * "BG=1" -- mirrors bruce_launcher__command_requests_background() in
 * modules/bruce_launcher/bruce_launcher_app.c. A one-shot CLI action with no
 * screen of its own (e.g. "BG=1 wifi connect") must not be tagged GUI=1. */
static bool input__kb_command_requests_background(const char *command) {
    const char *cursor = command;
    for (;;) {
        while (*cursor == ' ') cursor++;
        const char *token_end = cursor;
        while (*token_end != '\0' && *token_end != ' ') token_end++;
        size_t token_len = (size_t)(token_end - cursor);
        if (token_len == 0 || memchr(cursor, '=', token_len) == NULL) return false;
        if (token_len == 4 && strncmp(cursor, "BG=1", 4) == 0) return true;
        cursor = token_end;
    }
}

static void input__kb_run_hotkey(const char *action) {
    if (strcmp(action, "process switch next") == 0 || strcmp(action, "task switch next") == 0) {
        (void)process__switch_next();
        return;
    }

    while (isspace((unsigned char)*action)) action++;
    /* Every other hotkey action launches a foreground command that draws its
     * own screen (e.g. "process preview", "launcher"), so it needs GUI=1 the
     * same way a launcher menu entry does -- see
     * bruce_launcher__run_entry() -- unless it already requests one itself
     * or asks to run in the background. Without this, the launched
     * process's display context stays gui_requested=false, its viewport
     * stays hidden, and every draw call it makes is a silent no-op. */
    char command[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 8];
    if (strncmp(action, "GUI=", 4) == 0 || input__kb_command_requests_background(action)) {
        snprintf(command, sizeof(command), "%s", action);
    } else {
        snprintf(command, sizeof(command), "GUI=1 %s", action);
    }
    int result = app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);
    if (result < 0) ESP_LOGW(TAG, "hotkey action '%s' failed: %d", action, result);
}

void input_keyboard__init(void) {
    /* Outputs: push-pull, initially low. */
    for (int i = 0; i < INPUT__KB_OUT_COUNT; ++i) {
        (void)gpio__configure(s_kb_out_pins[i], BRUCE_GPIO_MODE_OUTPUT, BRUCE_GPIO_PULL_NONE);
        (void)gpio__write(s_kb_out_pins[i], 0);
    }

    /* Inputs: pull-up (keys are active-low).  Configure both primary and
     * alternate sense pins so the autodetect variant can switch at runtime. */
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) {
        (void)gpio__configure(s_kb_in_pins[i], BRUCE_GPIO_MODE_INPUT, BRUCE_GPIO_PULL_UP);
    }
#if INPUT__KB_AUTODETECT_ALT
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) {
        (void)gpio__configure(s_kb_in_alt_pins[i], BRUCE_GPIO_MODE_INPUT, BRUCE_GPIO_PULL_UP);
    }
#endif

    s_kb_initialized = true;
}

void input_keyboard__poll(void) {
    if (!s_kb_initialized) { return; }

    bool pressed[INPUT__KB_ROWS][INPUT__KB_COLS];
    (void)input__kb_scan(pressed);

    /* First pass: update modifier state. */
    input__kb_update_modifiers(pressed);

    uint64_t now = input__now_ms();
    char hotkey_action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1] = {0};
    for (int y = 0; y < INPUT__KB_ROWS; ++y) {
        for (int x = 0; x < INPUT__KB_COLS; ++x) {
            bool is_pressed = pressed[y][x];
            bool was_pressed = s_kb_prev_pressed[y][x];

            if (is_pressed && !was_pressed) {
                if (now - s_kb_last_event_ms[y][x] >= INPUT__DEBOUNCE_MS) {
                    s_kb_last_event_ms[y][x] = now;
                    bool is_modifier = input__kb_is_modifier(x, y);
                    char matched_action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1] = {0};
                    bool matched_hotkey =
                        !is_modifier &&
                        input__kb_match_hotkey(s_kb_normal[y][x], matched_action, sizeof(matched_action));

                    if (is_modifier) {
                        /* Modifier state is folded into normalized key events. */
                    } else if (matched_hotkey) {
                        memcpy(hotkey_action, matched_action, sizeof(hotkey_action));
                        s_kb_hotkey_consumed[y][x] = true;
                    } else if (s_kb_fn_held) {
                        if (y == 0 && x == 0) {
                            input__emit(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, '`', '`');
                        } else {
                            int32_t nav = input__kb_decode_fn_nav(x, y);
                            if (nav != 0) {
                                /* Keep Fn navigation distinguishable from punctuation
                                 * keys that share these legacy input codes. */
                                input__emit(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, nav, -1);
                            }
                        }
                    } else {
                        const char *normal_label = s_kb_normal[y][x];
                        const char *label = s_kb_shift_held ? s_kb_shifted[y][x] : normal_label;
                        int32_t code = input__kb_char_code(label);
                        if (y == 0 && x == 0 && !s_kb_shift_held) {
                            input__emit(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_BACK, -1);
                        } else {
                            if (s_kb_ctrl_held && code >= 'a' && code <= 'z') code &= 0x1f;
                            if (s_kb_ctrl_held && code >= 'A' && code <= 'Z') code &= 0x1f;
                            if (code != 0) { input__emit(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, code, code); }
                        }
                    }
                }
            }

            if (!is_pressed && was_pressed) {
                s_kb_last_event_ms[y][x] = now;
                /* Emit release events only for character keys (not for
                 * modifiers or Fn chords). */
                if (!input__kb_is_modifier(x, y)) {
                    if (s_kb_hotkey_consumed[y][x]) {
                        s_kb_hotkey_consumed[y][x] = false;
                    } else {
                        const char *label = s_kb_shift_held ? s_kb_shifted[y][x] : s_kb_normal[y][x];
                        int32_t code = input__kb_char_code(label);
                        if (code != 0) { input__emit(BRUCE_INPUT_KEY, BRUCE_INPUT_RELEASE, code, code); }
                    }
                }
            }

            s_kb_prev_pressed[y][x] = is_pressed;
        }
    }

    if (hotkey_action[0] != '\0') input__kb_run_hotkey(hotkey_action);
}

#else

void input_keyboard__init(void) {}
void input_keyboard__poll(void) {}

#endif
