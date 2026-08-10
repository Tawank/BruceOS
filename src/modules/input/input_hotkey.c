#include "input_hotkey.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_common.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/process.h"

#include "esp_log.h"

#define TAG "bruce_input_hotkey"

typedef struct {
    const char *name;
    int32_t code;
} input_hotkey__code_entry_t;

/* Canonical token -> code. Order matters only for input_hotkey__name_for_code(),
 * which returns the first match. */
static const input_hotkey__code_entry_t s_canonical_codes[] = {
    {"UP",           BRUCE_INPUT_CODE_UP          },
    {"DOWN",         BRUCE_INPUT_CODE_DOWN        },
    {"LEFT",         BRUCE_INPUT_CODE_LEFT        },
    {"RIGHT",        BRUCE_INPUT_CODE_RIGHT       },
    {"SELECT",       BRUCE_INPUT_CODE_SELECT      },
    {"BACK",         BRUCE_INPUT_CODE_BACK        },
    {"MENU",         BRUCE_INPUT_CODE_MENU        },
    {"HOME",         BRUCE_INPUT_CODE_HOME        },
    {"DELETE",       BRUCE_INPUT_CODE_DELETE      },
    {"PREV",         BRUCE_INPUT_CODE_PREV        },
    {"NEXT",         BRUCE_INPUT_CODE_NEXT        },
    {"BTN_A",        BRUCE_INPUT_CODE_BUTTON_A    },
    {"BTN_B",        BRUCE_INPUT_CODE_BUTTON_B    },
    {"BTN_C",        BRUCE_INPUT_CODE_BUTTON_C    },
    {"BTN_X",        BRUCE_INPUT_CODE_BUTTON_X    },
    {"BTN_Y",        BRUCE_INPUT_CODE_BUTTON_Y    },
    {"BTN_L1",       BRUCE_INPUT_CODE_BUTTON_L1   },
    {"BTN_R1",       BRUCE_INPUT_CODE_BUTTON_R1   },
    {"BTN_L2",       BRUCE_INPUT_CODE_BUTTON_L2   },
    {"BTN_R2",       BRUCE_INPUT_CODE_BUTTON_R2   },
    {"BTN_START",    BRUCE_INPUT_CODE_BUTTON_START},
    {"BTN_SELECT",   BRUCE_INPUT_CODE_BUTTON_SELECT},
    {"BTN_THUMB_L",  BRUCE_INPUT_CODE_BUTTON_THUMB_L},
    {"BTN_THUMB_R",  BRUCE_INPUT_CODE_BUTTON_THUMB_R},
};

void input_hotkey__split_duration(const char *key, uint32_t *out_hold_ms, const char **out_rest) {
    *out_hold_ms = 0;
    *out_rest = key;
    if (key == NULL) return;

    const char *cursor = key;
    while (isspace((unsigned char)*cursor)) cursor++;
    const char *digits_start = cursor;
    while (isdigit((unsigned char)*cursor)) cursor++;
    if (cursor == digits_start) return; /* no leading digits: not a duration prefix */

    unsigned long value = strtoul(digits_start, NULL, 10);
    uint32_t multiplier;
    if (strncmp(cursor, "ms", 2) == 0) {
        cursor += 2;
        multiplier = 1;
    } else if (*cursor == 's') {
        cursor += 1;
        multiplier = 1000;
    } else {
        return; /* digits not followed by a recognized unit: leave the whole string alone */
    }

    if (!isspace((unsigned char)*cursor)) return; /* the unit must be its own token */
    while (isspace((unsigned char)*cursor)) cursor++;
    if (*cursor == '\0') return; /* nothing left to bind to */

    *out_hold_ms = (uint32_t)(value * multiplier);
    *out_rest = cursor;
}

bool input_hotkey__code_for_name(const char *name, int32_t *out_code) {
    if (name == NULL) return false;
    for (size_t i = 0; i < sizeof(s_canonical_codes) / sizeof(s_canonical_codes[0]); ++i) {
        if (strcmp(s_canonical_codes[i].name, name) == 0) {
            *out_code = s_canonical_codes[i].code;
            return true;
        }
    }
    return false;
}

const char *input_hotkey__name_for_code(int32_t code) {
    for (size_t i = 0; i < sizeof(s_canonical_codes) / sizeof(s_canonical_codes[0]); ++i) {
        if (s_canonical_codes[i].code == code) return s_canonical_codes[i].name;
    }
    return NULL;
}

bool input_hotkey__find(
    const char *name, uint32_t *out_hold_ms, char *out_action, size_t action_size, size_t *out_index
) {
    if (name == NULL) return false;
    const bruce_config_hotkeys_t *hotkeys = config__get_hotkeys();
    if (hotkeys == NULL) return false;

    for (size_t i = 0; i < hotkeys->count; ++i) {
        uint32_t hold_ms = 0;
        const char *rest = NULL;
        input_hotkey__split_duration(hotkeys->items[i].key, &hold_ms, &rest);
        if (strcmp(rest, name) != 0) continue;

        int written = snprintf(out_action, action_size, "%s", hotkeys->items[i].action);
        if (written < 0 || (size_t)written >= action_size) return false;
        *out_hold_ms = hold_ms;
        if (out_index != NULL) *out_index = i;
        return true;
    }
    return false;
}

bool input_hotkey__find_by_hold(
    const char *name,
    bool want_hold,
    uint32_t *out_hold_ms,
    char *out_action,
    size_t action_size
) {
    if (name == NULL) return false;
    const bruce_config_hotkeys_t *hotkeys = config__get_hotkeys();
    if (hotkeys == NULL) return false;

    for (size_t i = 0; i < hotkeys->count; ++i) {
        uint32_t hold_ms = 0;
        const char *rest = NULL;
        input_hotkey__split_duration(hotkeys->items[i].key, &hold_ms, &rest);
        if (strcmp(rest, name) != 0 || (hold_ms > 0) != want_hold) continue;

        int written = snprintf(out_action, action_size, "%s", hotkeys->items[i].action);
        if (written < 0 || (size_t)written >= action_size) return false;
        *out_hold_ms = hold_ms;
        return true;
    }
    return false;
}

/* True iff `command`'s leading "key=value" environment tokens (the same
 * ones app_runner__run_command() parses off the front of the line) include
 * "BG=1" -- mirrors bruce_launcher__command_requests_background() in
 * modules/bruce_launcher/bruce_launcher_app.c. A one-shot CLI action with no
 * screen of its own (e.g. "BG=1 wifi connect") must not be tagged GUI=1. */
static bool input_hotkey__command_requests_background(const char *command) {
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

#define INPUT_HOTKEY__EMIT_PREFIX "emit "

/* Handles an "emit <NAME>" action. Returns true iff `action` was in that
 * form (regardless of whether NAME was recognized), so the caller never
 * falls through to treat it as an AppRunner command. */
static bool input_hotkey__run_emit(const char *action) {
    if (strncmp(action, INPUT_HOTKEY__EMIT_PREFIX, strlen(INPUT_HOTKEY__EMIT_PREFIX)) != 0) return false;

    const char *name = action + strlen(INPUT_HOTKEY__EMIT_PREFIX);
    while (isspace((unsigned char)*name)) name++;

    int32_t code;
    if (!input_hotkey__code_for_name(name, &code)) {
        ESP_LOGW(TAG, "hotkey remap '%s' has unknown target '%s'", action, name);
        return true;
    }
    input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, code, 1);
    input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, code, 0);
    return true;
}

void input_hotkey__run_action(const char *action) {
    if (action == NULL || action[0] == '\0') return;

    if (input_hotkey__run_emit(action)) return;

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
    if (strncmp(action, "GUI=", 4) == 0 || input_hotkey__command_requests_background(action)) {
        snprintf(command, sizeof(command), "%s", action);
    } else {
        snprintf(command, sizeof(command), "GUI=1 %s", action);
    }
    int result = app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);
    if (result < 0) ESP_LOGW(TAG, "hotkey action '%s' failed: %d", action, result);
}
