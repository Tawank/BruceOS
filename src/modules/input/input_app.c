#include "input_app.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/input.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#define TAG "bruce_input"

#ifndef INPUT__POLL_PERIOD_MS
#ifdef CONFIG_BRUCE_INPUT_POLL_PERIOD_MS
#define INPUT__POLL_PERIOD_MS CONFIG_BRUCE_INPUT_POLL_PERIOD_MS
#else
#define INPUT__POLL_PERIOD_MS 20
#endif
#endif

/* -------------------------------------------------------------------------- */
/* Board pinout and feature selection, from Kconfig (see src/Kconfig.projbuild). */
/* -------------------------------------------------------------------------- */

#define INPUT__HAS_BUTTON_0 CONFIG_BRUCE_BUTTON_SELECT_ENABLED
#if CONFIG_BRUCE_BUTTON_SELECT_ENABLED
#define INPUT__PIN_BUTTON_0 ((gpio_num_t)CONFIG_BRUCE_BUTTON_SELECT_GPIO)
#define INPUT__BUTTON_0_CODE BRUCE_INPUT_CODE_SELECT
#endif

#define INPUT__HAS_BUTTON_A CONFIG_BRUCE_BUTTON_A_ENABLED
#if CONFIG_BRUCE_BUTTON_A_ENABLED
#define INPUT__PIN_BUTTON_A ((gpio_num_t)CONFIG_BRUCE_BUTTON_A_GPIO)
#define INPUT__BUTTON_A_CODE BRUCE_INPUT_CODE_BUTTON_A
#endif

#define INPUT__HAS_BUTTON_B CONFIG_BRUCE_BUTTON_B_ENABLED
#if CONFIG_BRUCE_BUTTON_B_ENABLED
#define INPUT__PIN_BUTTON_B ((gpio_num_t)CONFIG_BRUCE_BUTTON_B_GPIO)
#define INPUT__BUTTON_B_CODE BRUCE_INPUT_CODE_BUTTON_B
#endif

#define INPUT__HAS_BUTTON_C CONFIG_BRUCE_BUTTON_C_ENABLED
#if CONFIG_BRUCE_BUTTON_C_ENABLED
#define INPUT__PIN_BUTTON_C ((gpio_num_t)CONFIG_BRUCE_BUTTON_C_GPIO)
#define INPUT__BUTTON_C_CODE BRUCE_INPUT_CODE_BUTTON_C
#endif

#define INPUT__HAS_BUTTON_D CONFIG_BRUCE_BUTTON_D_ENABLED
#if CONFIG_BRUCE_BUTTON_D_ENABLED
#define INPUT__PIN_BUTTON_D ((gpio_num_t)CONFIG_BRUCE_BUTTON_D_GPIO)
#define INPUT__BUTTON_D_CODE BRUCE_INPUT_CODE_BUTTON_X
#endif

#define INPUT__HAS_KEYBOARD CONFIG_BRUCE_KEYBOARD_ENABLED
#if CONFIG_BRUCE_KEYBOARD_ENABLED
#define INPUT__KB_OUT_PINS                                                                                   \
    {(gpio_num_t)CONFIG_BRUCE_KEYBOARD_OUT0_GPIO,                                                            \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_OUT1_GPIO,                                                            \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_OUT2_GPIO}
#define INPUT__KB_IN_PINS                                                                                    \
    {(gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN0_GPIO,                                                             \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN1_GPIO,                                                             \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN2_GPIO,                                                             \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN3_GPIO,                                                             \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN4_GPIO,                                                             \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN5_GPIO,                                                             \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN6_GPIO}
#define INPUT__KB_AUTODETECT_ALT CONFIG_BRUCE_KEYBOARD_AUTODETECT_ALT
#if CONFIG_BRUCE_KEYBOARD_AUTODETECT_ALT
#define INPUT__KB_IN_ALT_PINS                                                                                \
    {(gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN_ALT0_GPIO,                                                         \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN_ALT1_GPIO,                                                         \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN_ALT2_GPIO,                                                         \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN_ALT3_GPIO,                                                         \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN_ALT4_GPIO,                                                         \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN_ALT5_GPIO,                                                         \
     (gpio_num_t)CONFIG_BRUCE_KEYBOARD_IN_ALT6_GPIO}
#endif
#endif

/* -------------------------------------------------------------------------- */
/* Runtime configuration.                                                       */
/* -------------------------------------------------------------------------- */

#ifndef INPUT__DEBOUNCE_MS
#ifdef CONFIG_BRUCE_INPUT_DEBOUNCE_MS
#define INPUT__DEBOUNCE_MS CONFIG_BRUCE_INPUT_DEBOUNCE_MS
#else
#define INPUT__DEBOUNCE_MS 30
#endif
#endif

#ifndef INPUT__KB_SCAN_SETTLE_US
#ifdef CONFIG_BRUCE_INPUT_SCAN_SETTLE_US
#define INPUT__KB_SCAN_SETTLE_US CONFIG_BRUCE_INPUT_SCAN_SETTLE_US
#else
#define INPUT__KB_SCAN_SETTLE_US 30
#endif
#endif

/* -------------------------------------------------------------------------- */
/* Hardware helpers                                                           */
/* -------------------------------------------------------------------------- */

static uint64_t input__now_ms(void) { return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

static void input__emit(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value) {
    bruce_input_event_t event = {
        .type = type,
        .action = action,
        .code = code,
        .value = type == BRUCE_INPUT_KEY && value == 0 ? code : value,
    };
    (void)input__inject(&event);
}

/* -------------------------------------------------------------------------- */
/* GPIO buttons                                                               */
/* -------------------------------------------------------------------------- */

#if INPUT__HAS_BUTTON_A || INPUT__HAS_BUTTON_B || INPUT__HAS_BUTTON_C || INPUT__HAS_BUTTON_D ||              \
    INPUT__HAS_BUTTON_0

/* Per-button state. */
typedef struct {
    gpio_num_t pin;
    int32_t code;
    bool active_low;
    bool last_stable;
    uint64_t last_event_ms;
} input__button_t;

static input__button_t s_buttons[] = {
#if INPUT__HAS_BUTTON_A
    {INPUT__PIN_BUTTON_A, INPUT__BUTTON_A_CODE, true, true, 0},
#endif
#if INPUT__HAS_BUTTON_B
    {INPUT__PIN_BUTTON_B, INPUT__BUTTON_B_CODE, true, true, 0},
#endif
#if INPUT__HAS_BUTTON_C
    {INPUT__PIN_BUTTON_C, INPUT__BUTTON_C_CODE, true, true, 0},
#endif
#if INPUT__HAS_BUTTON_D
    {INPUT__PIN_BUTTON_D, INPUT__BUTTON_D_CODE, true, true, 0},
#endif
#if INPUT__HAS_BUTTON_0
    {INPUT__PIN_BUTTON_0, INPUT__BUTTON_0_CODE, true, true, 0},
#endif
};

static void input__buttons_init(void) {
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_buttons[i].pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = s_buttons[i].active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLDOWN_DISABLE,
            .pull_down_en = s_buttons[i].active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }
}

static void input__poll_buttons(void) {
    uint64_t now = input__now_ms();
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        input__button_t *btn = &s_buttons[i];
        bool raw = gpio_get_level(btn->pin) == 0;
        bool pressed = btn->active_low ? raw : !raw;

        if (pressed != btn->last_stable) {
            if (now - btn->last_event_ms >= INPUT__DEBOUNCE_MS) {
                btn->last_stable = pressed;
                btn->last_event_ms = now;
                input__emit(
                    BRUCE_INPUT_BUTTON,
                    pressed ? BRUCE_INPUT_PRESS : BRUCE_INPUT_RELEASE,
                    btn->code,
                    pressed ? 1 : 0
                );
            }
        }
    }
}

#else

static void input__buttons_init(void) {}
static void input__poll_buttons(void) {}

#endif

/* -------------------------------------------------------------------------- */
/* Cardputer keyboard matrix                                                  */
/* -------------------------------------------------------------------------- */

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

static const gpio_num_t s_kb_out_pins[INPUT__KB_OUT_COUNT] = INPUT__KB_OUT_PINS;
static const gpio_num_t s_kb_in_pins[INPUT__KB_IN_COUNT] = INPUT__KB_IN_PINS;
#if INPUT__KB_AUTODETECT_ALT
static const gpio_num_t s_kb_in_alt_pins[INPUT__KB_IN_COUNT] = INPUT__KB_IN_ALT_PINS;
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

static void input__kb_gpio_init(void) {
    /* Outputs: push-pull, initially low. */
    uint64_t out_mask = 0;
    for (int i = 0; i < INPUT__KB_OUT_COUNT; ++i) { out_mask |= 1ULL << s_kb_out_pins[i]; }
    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    for (int i = 0; i < INPUT__KB_OUT_COUNT; ++i) { gpio_set_level(s_kb_out_pins[i], 0); }

    /* Inputs: pull-up (keys are active-low).  Configure both primary and
     * alternate sense pins so the autodetect variant can switch at runtime. */
    uint64_t in_mask = 0;
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) { in_mask |= 1ULL << s_kb_in_pins[i]; }
#if INPUT__KB_AUTODETECT_ALT
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) { in_mask |= 1ULL << s_kb_in_alt_pins[i]; }
#endif
    gpio_config_t in_cfg = {
        .pin_bit_mask = in_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    s_kb_initialized = true;
}

static void input__kb_set_output(uint8_t scan_state) {
    scan_state &= 0x07;
    gpio_set_level(s_kb_out_pins[0], (scan_state & 0x01) ? 1 : 0);
    gpio_set_level(s_kb_out_pins[1], (scan_state & 0x02) ? 1 : 0);
    gpio_set_level(s_kb_out_pins[2], (scan_state & 0x04) ? 1 : 0);
}

static uint8_t input__kb_read_inputs(const gpio_num_t *pins) {
    uint8_t mask = 0;
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) {
        if (gpio_get_level(pins[i]) == 0) { mask |= (1U << i); }
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

static void input__kb_run_hotkey(const char *action) {
    if (strcmp(action, "process switch next") == 0 || strcmp(action, "task switch next") == 0) {
        (void)process__switch_next();
        return;
    }

    while (isspace((unsigned char)*action)) action++;
    int result;
    if (strcmp(action, "launcher") == 0) {
        const bruce_environment_variable_t gui_env[] = {
            {.name = "GUI", .value = "1"}
        };
        result = app_runner__run_with_environment("launcher", NULL, BRUCE_LAUNCH_FOREGROUND, gui_env, 1);
    } else {
        result = app_runner__run_command(action, BRUCE_LAUNCH_FOREGROUND);
    }
    if (result < 0) ESP_LOGW(TAG, "hotkey action '%s' failed: %d", action, result);
}

static void input__poll_keyboard(void) {
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

static void input__kb_gpio_init(void) {}
static void input__poll_keyboard(void) {}

#endif

/* -------------------------------------------------------------------------- */
/* Touchscreen (FT5x06/FT6336 family, I2C)                                    */
/* -------------------------------------------------------------------------- */

#if CONFIG_BRUCE_TOUCH_ENABLED

static esp_lcd_touch_handle_t s_touch;
static bool s_touch_prev_pressed;
static int32_t s_touch_prev_x;
static int32_t s_touch_prev_y;

static void input__touch_init(void) {
    i2c_master_bus_handle_t bus = board_i2c__acquire();
    if (bus == NULL) {
        ESP_LOGE(TAG, "touch: board I2C bus unavailable");
        return;
    }

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG(CONFIG_BRUCE_TOUCH_I2C_ADDR);
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_i2c(bus, &io_config, &io) != ESP_OK) {
        ESP_LOGE(TAG, "touch: failed to create I2C panel IO");
        return;
    }

    esp_lcd_touch_config_t touch_config = {
        .x_max = CONFIG_BRUCE_DISPLAY_WIDTH,
        .y_max = CONFIG_BRUCE_DISPLAY_HEIGHT,
        .rst_gpio_num = (gpio_num_t)CONFIG_BRUCE_TOUCH_PIN_RST,
        .int_gpio_num = (gpio_num_t)CONFIG_BRUCE_TOUCH_PIN_INT,
        .flags = {
                  .swap_xy = CONFIG_BRUCE_TOUCH_SWAP_XY,
                  .mirror_x = CONFIG_BRUCE_TOUCH_MIRROR_X,
                  .mirror_y = CONFIG_BRUCE_TOUCH_MIRROR_Y,
                  },
    };
    if (esp_lcd_touch_new_i2c_ft5x06(io, &touch_config, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch: failed to initialize FT5x06-family controller");
        s_touch = NULL;
    }
}

static void input__poll_touch(void) {
    if (s_touch == NULL) return;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return;

    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint16_t strength[1] = {0};
    uint8_t count = 0;
/* esp_lcd_touch_get_coordinates() is deprecated in favor of
 * esp_lcd_touch_get_data() as of newer esp_lcd_touch releases, but remains
 * functional; this is the simpler, longer-established API of the two. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    bool pressed = esp_lcd_touch_get_coordinates(s_touch, x, y, strength, &count, 1);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    if (pressed && !s_touch_prev_pressed) {
        input__emit(BRUCE_INPUT_TOUCH, BRUCE_INPUT_PRESS, x[0], y[0]);
    } else if (pressed && s_touch_prev_pressed &&
               ((int32_t)x[0] != s_touch_prev_x || (int32_t)y[0] != s_touch_prev_y)) {
        input__emit(BRUCE_INPUT_TOUCH, BRUCE_INPUT_CHANGE, x[0], y[0]);
    } else if (!pressed && s_touch_prev_pressed) {
        input__emit(BRUCE_INPUT_TOUCH, BRUCE_INPUT_RELEASE, s_touch_prev_x, s_touch_prev_y);
    }
    s_touch_prev_pressed = pressed;
    if (pressed) {
        s_touch_prev_x = x[0];
        s_touch_prev_y = y[0];
    }
}

#else

static void input__touch_init(void) {}
static void input__poll_touch(void) {}

#endif

/* -------------------------------------------------------------------------- */
/* Rotary encoder (quadrature, e.g. M5Stack DinMeter)                         */
/* -------------------------------------------------------------------------- */

#if CONFIG_BRUCE_ENCODER_ENABLED

#define INPUT__ENCODER_PIN_A ((gpio_num_t)CONFIG_BRUCE_ENCODER_PIN_A)
#define INPUT__ENCODER_PIN_B ((gpio_num_t)CONFIG_BRUCE_ENCODER_PIN_B)
/* One full detent is 4 quadrature sub-steps on a typical mechanical
 * encoder. */
#define INPUT__ENCODER_STEPS_PER_DETENT 4

/* Standard quadrature gray-code transition table: index is
 * (old_state << 2 | new_state), where each state is (A << 1 | B). Value is
 * -1/0/+1 sub-steps. This table (or an equivalent) shows up across most
 * embedded rotary-encoder decoders (e.g. PJRC's Encoder library, Ben
 * Buxton's Rotary library) - it's the well-known solution, not something
 * specific to any one board here. */
static const int8_t s_encoder_table[16] = {
    0,
    -1,
    1,
    0,
    1,
    0,
    0,
    -1,
    -1,
    0,
    0,
    1,
    0,
    1,
    -1,
    0,
};

static uint8_t s_encoder_prev_state;
static int s_encoder_accum;

static void input__encoder_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << INPUT__ENCODER_PIN_A) | (1ULL << INPUT__ENCODER_PIN_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    bool a = gpio_get_level(INPUT__ENCODER_PIN_A) == 0;
    bool b = gpio_get_level(INPUT__ENCODER_PIN_B) == 0;
    s_encoder_prev_state = (uint8_t)((a << 1) | b);
}

static void input__poll_encoder(void) {
    bool a = gpio_get_level(INPUT__ENCODER_PIN_A) == 0;
    bool b = gpio_get_level(INPUT__ENCODER_PIN_B) == 0;
    uint8_t new_state = (uint8_t)((a << 1) | b);
    int8_t step = s_encoder_table[(s_encoder_prev_state << 2) | new_state];
    s_encoder_prev_state = new_state;
    if (step == 0) return;

    /* Clockwise vs counter-clockwise -> DOWN vs UP is a guess (not verified
     * against real hardware); if a board's encoder feels reversed, swap the
     * two blocks below rather than the A/B pins. */
    s_encoder_accum += step;
    while (s_encoder_accum >= INPUT__ENCODER_STEPS_PER_DETENT) {
        s_encoder_accum -= INPUT__ENCODER_STEPS_PER_DETENT;
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_DOWN, 1);
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, BRUCE_INPUT_CODE_DOWN, 0);
    }
    while (s_encoder_accum <= -INPUT__ENCODER_STEPS_PER_DETENT) {
        s_encoder_accum += INPUT__ENCODER_STEPS_PER_DETENT;
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_UP, 1);
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, BRUCE_INPUT_CODE_UP, 0);
    }
}

#else

static void input__encoder_init(void) {}
static void input__poll_encoder(void) {}

#endif

/* -------------------------------------------------------------------------- */
/* Public module adapter                                                       */
/* -------------------------------------------------------------------------- */

static bruce_result_t input_app__init(void) {
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    input__buttons_init();
    input__kb_gpio_init();
    input__touch_init();
    input__encoder_init();
#endif
    return BRUCE_OK;
}

static void input_app__poll(void) {
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    input__poll_buttons();
    input__poll_keyboard();
    input__poll_touch();
    input__poll_encoder();
#endif
}

int input_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    bruce_result_t result = input_app__init();
    if (result != BRUCE_OK) { return result; }

    while (process__current_signal() == 0) {
        input_app__poll();
        result = runtime__sleep(INPUT__POLL_PERIOD_MS);
        if (result != BRUCE_OK && process__current_signal() == 0) { return result; }
    }
    return BRUCE_OK;
}
