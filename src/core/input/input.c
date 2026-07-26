#include "input.h"

#include "core_sdk/input.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"
#include "core/task/task.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "bruce_input"

/* -------------------------------------------------------------------------- */
/* Board-specific pinout and feature selection.  Defaults come from the     */
/* M5Stack Cardputer and StickC Plus2 reference schematics.                     */
/* -------------------------------------------------------------------------- */

#if defined(CONFIG_BRUCE_BOARD_M5_CARDPUTER)
#    define INPUT__HAS_BUTTON_0         1
#    define INPUT__PIN_BUTTON_0         GPIO_NUM_0
#    define INPUT__BUTTON_0_CODE        BRUCE_INPUT_CODE_SELECT
#    define INPUT__HAS_KEYBOARD         1
#    define INPUT__KB_OUT_PINS          {GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11}
#    define INPUT__KB_IN_PINS           {GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7}
#    define INPUT__KB_IN_ALT_PINS       {GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7}
#    define INPUT__KB_AUTODETECT_ALT    1
#elif defined(CONFIG_BRUCE_BOARD_M5_STICKC_PLUS2)
#    define INPUT__HAS_BUTTON_A         1
#    define INPUT__PIN_BUTTON_A         GPIO_NUM_37
#    define INPUT__BUTTON_A_CODE        BRUCE_INPUT_CODE_BUTTON_A
#    define INPUT__HAS_BUTTON_B         1
#    define INPUT__PIN_BUTTON_B         GPIO_NUM_39
#    define INPUT__BUTTON_B_CODE        BRUCE_INPUT_CODE_BUTTON_B
#    define INPUT__HAS_BUTTON_C         1
#    define INPUT__PIN_BUTTON_C         GPIO_NUM_35
#    define INPUT__BUTTON_C_CODE        BRUCE_INPUT_CODE_BUTTON_C
#else
#    define INPUT__NO_HARDWARE            1
#endif

/* Ensure all feature flags are defined for #if use. */
#ifndef INPUT__HAS_BUTTON_0
#    define INPUT__HAS_BUTTON_0 0
#endif
#ifndef INPUT__HAS_BUTTON_A
#    define INPUT__HAS_BUTTON_A 0
#endif
#ifndef INPUT__HAS_BUTTON_B
#    define INPUT__HAS_BUTTON_B 0
#endif
#ifndef INPUT__HAS_BUTTON_C
#    define INPUT__HAS_BUTTON_C 0
#endif
#ifndef INPUT__HAS_KEYBOARD
#    define INPUT__HAS_KEYBOARD 0
#endif
#ifndef INPUT__KB_AUTODETECT_ALT
#    define INPUT__KB_AUTODETECT_ALT 0
#endif

/* -------------------------------------------------------------------------- */
/* Runtime configuration.                                                       */
/* -------------------------------------------------------------------------- */

#ifndef INPUT__TASK_STACK
#    ifdef CONFIG_BRUCE_INPUT_TASK_STACK
#        define INPUT__TASK_STACK CONFIG_BRUCE_INPUT_TASK_STACK
#    else
#        define INPUT__TASK_STACK 8192
#    endif
#endif

#ifndef INPUT__TASK_PRIORITY
#    define INPUT__TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#endif

#ifndef INPUT__POLL_PERIOD_MS
#    ifdef CONFIG_BRUCE_INPUT_POLL_PERIOD_MS
#        define INPUT__POLL_PERIOD_MS CONFIG_BRUCE_INPUT_POLL_PERIOD_MS
#    else
#        define INPUT__POLL_PERIOD_MS 20
#    endif
#endif

#ifndef INPUT__DEBOUNCE_MS
#    ifdef CONFIG_BRUCE_INPUT_DEBOUNCE_MS
#        define INPUT__DEBOUNCE_MS CONFIG_BRUCE_INPUT_DEBOUNCE_MS
#    else
#        define INPUT__DEBOUNCE_MS 30
#    endif
#endif

#ifndef INPUT__KB_SCAN_SETTLE_US
#    ifdef CONFIG_BRUCE_INPUT_SCAN_SETTLE_US
#        define INPUT__KB_SCAN_SETTLE_US CONFIG_BRUCE_INPUT_SCAN_SETTLE_US
#    else
#        define INPUT__KB_SCAN_SETTLE_US 30
#    endif
#endif

#ifndef INPUT__KB_REPEAT_DELAY_MS
#    define INPUT__KB_REPEAT_DELAY_MS 400
#endif

#ifndef INPUT__KB_REPEAT_PERIOD_MS
#    define INPUT__KB_REPEAT_PERIOD_MS 100
#endif

/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */

#define INPUT__QUEUE_LENGTH 16

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static StaticQueue_t s_queue_storage;
static uint8_t s_queue_buffer[INPUT__QUEUE_LENGTH * sizeof(bruce_input_event_t)];
static QueueHandle_t s_queue;
static bool s_initialized;
static TaskHandle_t s_poll_task;
static bruce_task_id_t s_foreground_task_id;
static uint32_t s_foreground_epoch;

static inline void input__lock(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

static inline void input__unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

/* -------------------------------------------------------------------------- */
/* Hardware helpers                                                           */
/* -------------------------------------------------------------------------- */

static uint64_t input__now_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static bool input__caller_is_foreground_locked(bruce_task_id_t caller)
{
    return caller != BRUCE_TASK_ID_INVALID && caller == s_foreground_task_id;
}

/* Push one event into the queue.  Caller must hold the lock. */
static bruce_result_t input__push_event_locked(bruce_input_type_t type, bruce_input_action_t action,
                                               int32_t code, int32_t value, bruce_task_id_t source_task_id)
{
    if (s_queue == NULL) {
        return BRUCE_ERR_INVALID_STATE;
    }

    bruce_input_event_t event = {
        .type = type,
        .action = action,
        .code = code,
    };

    if (type == BRUCE_INPUT_KEY) {
        /* For keys, `value` carries the ASCII/character code if non-zero,
         * otherwise the code itself is the character code. */
        event.value = value != 0 ? value : code;
    } else {
        event.value = value;
    }

    event.timestamp_ms = input__now_ms();
    event.source_task_id = source_task_id;

    BaseType_t sent = xQueueSend(s_queue, &event, 0);
    return sent == pdPASS ? BRUCE_OK : BRUCE_ERR_BUSY;
}

/* -------------------------------------------------------------------------- */
/* GPIO buttons                                                               */
/* -------------------------------------------------------------------------- */

#if INPUT__HAS_BUTTON_A || INPUT__HAS_BUTTON_B || INPUT__HAS_BUTTON_C || INPUT__HAS_BUTTON_0

/* Per-button state. */
typedef struct {
    gpio_num_t pin;
    int32_t code;
    bool active_low;
    bool last_stable;
    uint64_t last_event_ms;
} input__button_t;

static input__button_t s_buttons[] = {
#    if INPUT__HAS_BUTTON_A
    {INPUT__PIN_BUTTON_A, INPUT__BUTTON_A_CODE, true, true, 0},
#    endif
#    if INPUT__HAS_BUTTON_B
    {INPUT__PIN_BUTTON_B, INPUT__BUTTON_B_CODE, true, true, 0},
#    endif
#    if INPUT__HAS_BUTTON_C
    {INPUT__PIN_BUTTON_C, INPUT__BUTTON_C_CODE, true, true, 0},
#    endif
#    if INPUT__HAS_BUTTON_0
    {INPUT__PIN_BUTTON_0, INPUT__BUTTON_0_CODE, true, true, 0},
#    endif
};

static void input__buttons_init(void)
{
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

static void input__poll_buttons(void)
{
    uint64_t now = input__now_ms();
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        input__button_t *btn = &s_buttons[i];
        bool raw = gpio_get_level(btn->pin) == 0;
        bool pressed = btn->active_low ? raw : !raw;

        if (pressed != btn->last_stable) {
            if (now - btn->last_event_ms >= INPUT__DEBOUNCE_MS) {
                btn->last_stable = pressed;
                btn->last_event_ms = now;
                input__lock();
                (void)input__push_event_locked(BRUCE_INPUT_BUTTON, pressed ? BRUCE_INPUT_PRESS : BRUCE_INPUT_RELEASE,
                                               btn->code, pressed ? 1 : 0, BRUCE_TASK_ID_INVALID);
                bruce_task_id_t owner = s_foreground_task_id;
                input__unlock();
                task_registry__input_wake(owner);
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

#    define INPUT__KB_OUT_COUNT 3
#    define INPUT__KB_IN_COUNT 7
#    define INPUT__KB_COLS 14
#    define INPUT__KB_ROWS 4

/* Visual layout (row 0 is the top `123... row). */
static const char *const s_kb_normal[INPUT__KB_ROWS][INPUT__KB_COLS] = {
    {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "del"},
    {"tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\"},
    {"fn", "shift", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "enter"},
    {"ctrl", "opt", "alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "space"},
};

static const char *const s_kb_shifted[INPUT__KB_ROWS][INPUT__KB_COLS] = {
    {"~", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "+", "del"},
    {"tab", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}", "|"},
    {"fn", "shift", "A", "S", "D", "F", "G", "H", "J", "K", "L", ":", "\"", "enter"},
    {"ctrl", "opt", "alt", "Z", "X", "C", "V", "B", "N", "M", "<", ">", "?", "space"},
};

static const gpio_num_t s_kb_out_pins[INPUT__KB_OUT_COUNT] = INPUT__KB_OUT_PINS;
static const gpio_num_t s_kb_in_pins[INPUT__KB_IN_COUNT] = INPUT__KB_IN_PINS;
#    if INPUT__KB_AUTODETECT_ALT
static const gpio_num_t s_kb_in_alt_pins[INPUT__KB_IN_COUNT] = INPUT__KB_IN_ALT_PINS;
#    endif

static bool s_kb_use_alt_in_pins;
static bool s_kb_prev_pressed[INPUT__KB_ROWS][INPUT__KB_COLS];
static uint64_t s_kb_last_event_ms[INPUT__KB_ROWS][INPUT__KB_COLS];
static bool s_kb_fn_held;
static bool s_kb_shift_held;
static bool s_kb_ctrl_held;
static bool s_kb_alt_held;

static bool s_kb_initialized;

static void input__kb_gpio_init(void)
{
    /* Outputs: push-pull, initially low. */
    uint64_t out_mask = 0;
    for (int i = 0; i < INPUT__KB_OUT_COUNT; ++i) {
        out_mask |= 1ULL << s_kb_out_pins[i];
    }
    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    for (int i = 0; i < INPUT__KB_OUT_COUNT; ++i) {
        gpio_set_level(s_kb_out_pins[i], 0);
    }

    /* Inputs: pull-up (keys are active-low).  Configure both primary and
     * alternate sense pins so the autodetect variant can switch at runtime. */
    uint64_t in_mask = 0;
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) {
        in_mask |= 1ULL << s_kb_in_pins[i];
    }
#    if INPUT__KB_AUTODETECT_ALT
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) {
        in_mask |= 1ULL << s_kb_in_alt_pins[i];
    }
#    endif
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

static void input__kb_set_output(uint8_t scan_state)
{
    scan_state &= 0x07;
    gpio_set_level(s_kb_out_pins[0], (scan_state & 0x01) ? 1 : 0);
    gpio_set_level(s_kb_out_pins[1], (scan_state & 0x02) ? 1 : 0);
    gpio_set_level(s_kb_out_pins[2], (scan_state & 0x04) ? 1 : 0);
}

static uint8_t input__kb_read_inputs(const gpio_num_t *pins)
{
    uint8_t mask = 0;
    for (int i = 0; i < INPUT__KB_IN_COUNT; ++i) {
        if (gpio_get_level(pins[i]) == 0) {
            mask |= (1U << i);
        }
    }
    return mask;
}

static bool input__kb_scan(bool out_pressed[INPUT__KB_ROWS][INPUT__KB_COLS])
{
    bool any = false;
    memset(out_pressed, 0, sizeof(bool) * INPUT__KB_ROWS * INPUT__KB_COLS);

    for (int scan_state = 0; scan_state < 8; ++scan_state) {
        input__kb_set_output(scan_state);
        esp_rom_delay_us(INPUT__KB_SCAN_SETTLE_US);

        uint8_t in_mask = input__kb_read_inputs(s_kb_use_alt_in_pins ? s_kb_in_alt_pins : s_kb_in_pins);

#    if INPUT__KB_AUTODETECT_ALT
        /* If primary pins show nothing, try the alternate IN0/IN1 pins. */
        if (in_mask == 0 && !s_kb_use_alt_in_pins) {
            uint8_t alt_mask = input__kb_read_inputs(s_kb_in_alt_pins);
            if (alt_mask != 0) {
                s_kb_use_alt_in_pins = true;
                ESP_LOGW(TAG, "keyboard switched to alternate IN0/IN1 pins");
                in_mask = alt_mask;
            }
        }
#    endif

        if (in_mask == 0) {
            continue;
        }

        for (int j = 0; j < INPUT__KB_IN_COUNT; ++j) {
            if ((in_mask & (1U << j)) == 0) {
                continue;
            }
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

static int32_t input__kb_char_code(const char *label)
{
    if (label == NULL) {
        return 0;
    }
    if (strcmp(label, "space") == 0) {
        return ' ';
    }
    if (strcmp(label, "enter") == 0) {
        return '\n';
    }
    if (strcmp(label, "tab") == 0) {
        return '\t';
    }
    if (strcmp(label, "del") == 0) {
        return '\b';
    }
    if (label[0] != '\0' && label[1] == '\0') {
        return (int32_t)(unsigned char)label[0];
    }
    return 0;
}

static bool input__kb_is_modifier(int x, int y)
{
    const char *label = s_kb_normal[y][x];
    return strcmp(label, "fn") == 0 || strcmp(label, "shift") == 0 || strcmp(label, "ctrl") == 0 ||
           strcmp(label, "alt") == 0;
}

static void input__kb_update_modifiers(const bool pressed[INPUT__KB_ROWS][INPUT__KB_COLS])
{
    s_kb_fn_held = false;
    s_kb_shift_held = false;
    s_kb_ctrl_held = false;
    s_kb_alt_held = false;

    for (int y = 0; y < INPUT__KB_ROWS; ++y) {
        for (int x = 0; x < INPUT__KB_COLS; ++x) {
            if (!pressed[y][x]) {
                continue;
            }
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
static int32_t input__kb_decode_fn_nav(int x, int y)
{
    /* Cardputer captured bindings:
     *   NavUp    : Fn + ;   (row 2, col 11)
     *   NavDown  : Fn + .   (row 3, col 11)
     *   NavLeft  : Fn + ,   (row 3, col 10)
     *   NavRight : Fn + /   (row 3, col 12)
     *   Back/Esc : Fn + 1   (row 0, col 1)
     *   Delete   : Fn + del (row 0, col 13)
     */
    if (y == 2 && x == 11) {
        return BRUCE_INPUT_CODE_UP;
    }
    if (y == 3 && x == 11) {
        return BRUCE_INPUT_CODE_DOWN;
    }
    if (y == 3 && x == 10) {
        return BRUCE_INPUT_CODE_LEFT;
    }
    if (y == 3 && x == 12) {
        return BRUCE_INPUT_CODE_RIGHT;
    }
    if (y == 0 && x == 1) {
        return BRUCE_INPUT_CODE_BACK;
    }
    if (y == 0 && x == 13) {
        return BRUCE_INPUT_CODE_BACK;
    }
    return 0;
}

static void input__poll_keyboard(void)
{
    if (!s_kb_initialized) {
        return;
    }

    bool pressed[INPUT__KB_ROWS][INPUT__KB_COLS];
    (void)input__kb_scan(pressed);

    /* First pass: update modifier state. */
    input__kb_update_modifiers(pressed);

    uint64_t now = input__now_ms();
    input__lock();

    for (int y = 0; y < INPUT__KB_ROWS; ++y) {
        for (int x = 0; x < INPUT__KB_COLS; ++x) {
            bool is_pressed = pressed[y][x];
            bool was_pressed = s_kb_prev_pressed[y][x];

            if (is_pressed && !was_pressed) {
                if (now - s_kb_last_event_ms[y][x] >= INPUT__DEBOUNCE_MS) {
                    s_kb_last_event_ms[y][x] = now;

                    if (input__kb_is_modifier(x, y)) {
                        /* Modifiers are tracked internally; optionally emit
                         * a key press with a zero code so listeners can see
                         * the modifier. */
                        const char *label = s_kb_normal[y][x];
                        (void)input__push_event_locked(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, 0,
                                                       (int32_t)label[0], BRUCE_TASK_ID_INVALID);
                    } else if (s_kb_fn_held) {
                        int32_t nav = input__kb_decode_fn_nav(x, y);
                        if (nav != 0) {
                            (void)input__push_event_locked(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, nav, 0,
                                                           BRUCE_TASK_ID_INVALID);
                        }
                    } else {
                        const char *label = s_kb_shift_held ? s_kb_shifted[y][x] : s_kb_normal[y][x];
                        int32_t code = input__kb_char_code(label);
                        if (code != 0) {
                            (void)input__push_event_locked(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, code, code,
                                                           BRUCE_TASK_ID_INVALID);
                        }
                    }
                }
            }

            if (!is_pressed && was_pressed) {
                s_kb_last_event_ms[y][x] = now;
                /* Emit release events only for character keys (not for
                 * modifiers or Fn chords). */
                if (!input__kb_is_modifier(x, y)) {
                    const char *label = s_kb_shift_held ? s_kb_shifted[y][x] : s_kb_normal[y][x];
                    int32_t code = input__kb_char_code(label);
                    if (code != 0) {
                        (void)input__push_event_locked(BRUCE_INPUT_KEY, BRUCE_INPUT_RELEASE, code, code,
                                                       BRUCE_TASK_ID_INVALID);
                    }
                }
            }

            s_kb_prev_pressed[y][x] = is_pressed;
        }
    }

    bruce_task_id_t owner = s_foreground_task_id;
    input__unlock();
    task_registry__input_wake(owner);
}

#else

static void input__kb_gpio_init(void) {}
static void input__poll_keyboard(void) {}

#endif

/* -------------------------------------------------------------------------- */
/* Polling task                                                               */
/* -------------------------------------------------------------------------- */

static void input__poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        input__poll_buttons();
        input__poll_keyboard();
        vTaskDelay(pdMS_TO_TICKS(INPUT__POLL_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bruce_result_t input__init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutex_storage);
    }

    input__lock();
    if (s_initialized) {
        input__unlock();
        return BRUCE_OK;
    }

    s_queue = xQueueCreateStatic(INPUT__QUEUE_LENGTH, sizeof(bruce_input_event_t), s_queue_buffer,
                                  &s_queue_storage);
    if (s_queue == NULL) {
        input__unlock();
        return BRUCE_ERR_INTERNAL;
    }

    input__buttons_init();
    input__kb_gpio_init();

    BaseType_t created = xTaskCreate(input__poll_task, "bruce_input", INPUT__TASK_STACK, NULL,
                                    INPUT__TASK_PRIORITY, &s_poll_task);
    if (created != pdPASS) {
        s_poll_task = NULL;
        input__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    s_initialized = true;
    input__unlock();
    return BRUCE_OK;
}

void input__deinit(void)
{
    if (s_mutex == NULL) {
        return;
    }
    input__lock();
    if (!s_initialized) {
        input__unlock();
        return;
    }

    if (s_poll_task != NULL) {
        vTaskDelete(s_poll_task);
        s_poll_task = NULL;
    }

    s_initialized = false;
    bruce_task_id_t owner = s_foreground_task_id;
    s_foreground_task_id = BRUCE_TASK_ID_INVALID;
    s_foreground_epoch++;
    input__unlock();
    task_registry__input_wake(owner);
}

void input__foreground_changed(bruce_task_id_t task_id)
{
    if (s_mutex == NULL) {
        s_foreground_task_id = task_id;
        s_foreground_epoch++;
        return;
    }
    input__lock();
    if (s_foreground_task_id != task_id) {
        s_foreground_task_id = task_id;
        s_foreground_epoch++;
    }
    input__unlock();
}

bruce_result_t input__read(bruce_input_event_t *out_event, uint32_t timeout_ms)
{
    if (out_event == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_task_id_t caller = task__current_id();
    uint64_t start_ms = input__now_ms();
    input__lock();
    if (!s_initialized) {
        input__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (!input__caller_is_foreground_locked(caller)) {
        input__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    uint32_t epoch = s_foreground_epoch;
    input__unlock();

    for (;;) {
        if (task_registry__input_wake_clear(caller) != BRUCE_OK) {
            return BRUCE_ERR_NOT_FOREGROUND;
        }
        input__lock();
        if (!s_initialized) {
            input__unlock();
            return BRUCE_ERR_NOT_INITIALIZED;
        }
        if (!input__caller_is_foreground_locked(caller) || s_foreground_epoch != epoch) {
            input__unlock();
            return BRUCE_ERR_NOT_FOREGROUND;
        }
        BaseType_t received = xQueueReceive(s_queue, out_event, 0);
        input__unlock();
        if (received == pdPASS) {
            return BRUCE_OK;
        }
        if (timeout_ms == 0) {
            return BRUCE_ERR_TIMEOUT;
        }
        uint32_t remaining = portMAX_DELAY;
        if (timeout_ms != portMAX_DELAY) {
            uint64_t elapsed = input__now_ms() - start_ms;
            if (elapsed >= timeout_ms) {
                return BRUCE_ERR_TIMEOUT;
            }
            remaining = (uint32_t)(timeout_ms - elapsed);
        }
        (void)task_registry__input_wake_wait(caller, remaining);
    }
}

bruce_result_t input__flush(void)
{
    bruce_task_id_t caller = task__current_id();
    input__lock();
    if (!s_initialized) {
        input__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (!input__caller_is_foreground_locked(caller)) {
        input__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    bruce_input_event_t ev;
    while (xQueueReceive(s_queue, &ev, 0) == pdPASS) {
        /* Discard. */
    }
    input__unlock();
    return BRUCE_OK;
}

bruce_result_t input__peek(bruce_input_event_t *out_event)
{
    if (out_event == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_task_id_t caller = task__current_id();
    input__lock();
    if (!s_initialized) {
        input__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (!input__caller_is_foreground_locked(caller)) {
        input__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    BaseType_t received = xQueuePeek(s_queue, out_event, 0);
    input__unlock();
    if (received != pdPASS) {
        return BRUCE_ERR_TIMEOUT;
    }
    return BRUCE_OK;
}

bruce_result_t input__wait(uint32_t timeout_ms, int32_t *out_code)
{
    if (out_code == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    uint64_t start_ms = input__now_ms();
    for (;;) {
        uint32_t remaining;
        if (timeout_ms == portMAX_DELAY) {
            remaining = portMAX_DELAY;
        } else {
            uint64_t elapsed_ms = input__now_ms() - start_ms;
            if (elapsed_ms >= timeout_ms) {
                remaining = 0;
            } else {
                remaining = (uint32_t)(timeout_ms - elapsed_ms);
            }
        }

        bruce_input_event_t ev;
        bruce_result_t rc = input__read(&ev, remaining);
        if (rc != BRUCE_OK) {
            return rc;
        }
        if (ev.action == BRUCE_INPUT_PRESS) {
            *out_code = ev.code;
            return BRUCE_OK;
        }
    }
}

bool input__check(int32_t code, bool consume)
{
    bruce_task_id_t caller = task__current_id();
    input__lock();
    if (!s_initialized || !input__caller_is_foreground_locked(caller)) {
        input__unlock();
        return false;
    }

    UBaseType_t count = uxQueueMessagesWaiting(s_queue);
    if (count == 0 || count > INPUT__QUEUE_LENGTH) {
        input__unlock();
        return false;
    }

    bruce_input_event_t buffer[INPUT__QUEUE_LENGTH];
    for (UBaseType_t i = 0; i < count; ++i) {
        (void)xQueueReceive(s_queue, &buffer[i], 0);
    }

    bool found = false;
    UBaseType_t found_index = 0;
    for (UBaseType_t i = 0; i < count; ++i) {
        if (buffer[i].action == BRUCE_INPUT_PRESS && buffer[i].code == code) {
            found = true;
            found_index = i;
            break;
        }
    }

    if (found && consume) {
        /* Restore everything except the consumed event, preserving order. */
        for (UBaseType_t i = count; i-- > 0;) {
            if (i == found_index) {
                continue;
            }
            (void)xQueueSendToFront(s_queue, &buffer[i], 0);
        }
    } else {
        /* Restore all events in original order. */
        for (UBaseType_t i = count; i-- > 0;) {
            (void)xQueueSendToFront(s_queue, &buffer[i], 0);
        }
    }

    input__unlock();
    return found;
}

bruce_result_t input__inject(const bruce_input_event_t *event)
{
    if (event == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (event->type != BRUCE_INPUT_KEY && event->type != BRUCE_INPUT_BUTTON && event->type != BRUCE_INPUT_TOUCH &&
        event->type != BRUCE_INPUT_ENCODER && event->type != BRUCE_INPUT_CUSTOM) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (event->action != BRUCE_INPUT_PRESS && event->action != BRUCE_INPUT_RELEASE && event->action != BRUCE_INPUT_CHANGE) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t perm = permission__check(BRUCE_PERMISSION_INPUT);
    if (perm != BRUCE_OK) {
        return perm;
    }

    bruce_input_event_t ev = *event;
    ev.timestamp_ms = input__now_ms();
    ev.source_task_id = task__current_id();

    input__lock();
    if (!s_initialized) {
        input__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    BaseType_t sent = xQueueSend(s_queue, &ev, 0);
    bruce_task_id_t owner = s_foreground_task_id;
    input__unlock();
    task_registry__input_wake(owner);
    return sent == pdPASS ? BRUCE_OK : BRUCE_ERR_BUSY;
}
