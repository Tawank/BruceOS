#include "input_test.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "modules/input/input_hotkey.h"

#include "core_sdk/config.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"

bool selftest__run_input_poll_case(void) {
    bruce_input_event_t ev;
    bruce_result_t result = input__poll(&ev);
    if (result == BRUCE_ERR_TIMEOUT) {
        printf("[selftest] input/poll: OK (empty queue returns TIMEOUT)\n");
        return true;
    }
    printf("[selftest] input/poll: FAIL, expected TIMEOUT, got %d\n", result);
    return false;
}

bool selftest__run_input_inject_case(void) {
    bruce_input_event_t inject = {
        .type = BRUCE_INPUT_BUTTON,
        .action = BRUCE_INPUT_PRESS,
        .code = BRUCE_INPUT_CODE_SELECT,
        .value = 1,
    };

    bruce_result_t result = input__inject(&inject);
    if (result != BRUCE_OK) {
        printf("[selftest] input/inject: FAIL, inject returned %d\n", result);
        return false;
    }

    bruce_input_event_t ev;
    result = input__read(&ev, 1000);
    if (result != BRUCE_OK) {
        printf("[selftest] input/inject: FAIL, read returned %d\n", result);
        return false;
    }

    if (ev.type != BRUCE_INPUT_BUTTON || ev.action != BRUCE_INPUT_PRESS ||
        ev.code != BRUCE_INPUT_CODE_SELECT || ev.value != 1) {
        printf(
            "[selftest] input/inject: FAIL, event mismatch type=%d action=%d code=%" PRId32 " value=%" PRId32
            "\n",
            ev.type,
            ev.action,
            ev.code,
            ev.value
        );
        return false;
    }

    printf("[selftest] input/inject: OK\n");
    return true;
}

bool selftest__run_input_flush_case(void) {
    /* Inject two events, then flush, then read should return TIMEOUT. */
    bruce_input_event_t inject = {
        .type = BRUCE_INPUT_BUTTON,
        .action = BRUCE_INPUT_PRESS,
        .code = BRUCE_INPUT_CODE_BACK,
        .value = 1,
    };
    (void)input__inject(&inject);
    (void)input__inject(&inject);

    bruce_result_t result = input__flush();
    if (result != BRUCE_OK) {
        printf("[selftest] input/flush: FAIL, flush returned %d\n", result);
        return false;
    }

    bruce_input_event_t ev;
    result = input__poll(&ev);
    if (result != BRUCE_ERR_TIMEOUT) {
        printf("[selftest] input/flush: FAIL, poll after flush returned %d\n", result);
        return false;
    }

    printf("[selftest] input/flush: OK\n");
    return true;
}

bool selftest__run_input_non_blocking_case(void) {
    /* Flush any stale events, then ensure non-blocking read returns TIMEOUT. */
    (void)input__flush();

    bruce_input_event_t ev;
    bruce_result_t result = input__read(&ev, 0);
    if (result != BRUCE_ERR_TIMEOUT) {
        printf("[selftest] input/nonblocking: FAIL, read(0) returned %d\n", result);
        return false;
    }

    /* Inject and read back with timeout=0. */
    bruce_input_event_t inject = {
        .type = BRUCE_INPUT_KEY,
        .action = BRUCE_INPUT_PRESS,
        .code = 'a',
        .value = 'a',
    };
    result = input__inject(&inject);
    if (result != BRUCE_OK) {
        printf("[selftest] input/nonblocking: FAIL, inject returned %d\n", result);
        return false;
    }

    result = input__read(&ev, 0);
    if (result != BRUCE_OK || ev.code != 'a' || ev.value != 'a') {
        printf(
            "[selftest] input/nonblocking: FAIL, read returned %d, code=%" PRId32 " value=%" PRId32 "\n",
            result,
            ev.code,
            ev.value
        );
        return false;
    }

    printf("[selftest] input/nonblocking: OK\n");
    return true;
}

bool selftest__run_input_peek_case(void) {
    (void)input__flush();

    bruce_input_event_t ev;
    bruce_result_t result = input__peek(&ev);
    if (result != BRUCE_ERR_TIMEOUT) {
        printf("[selftest] input/peek: FAIL, expected TIMEOUT on empty queue, got %d\n", result);
        return false;
    }

    bruce_input_event_t inject = {
        .type = BRUCE_INPUT_BUTTON,
        .action = BRUCE_INPUT_PRESS,
        .code = BRUCE_INPUT_CODE_DOWN,
        .value = 1,
    };
    result = input__inject(&inject);
    if (result != BRUCE_OK) {
        printf("[selftest] input/peek: FAIL, inject returned %d\n", result);
        return false;
    }

    result = input__peek(&ev);
    if (result != BRUCE_OK || ev.code != BRUCE_INPUT_CODE_DOWN) {
        printf("[selftest] input/peek: FAIL, peek returned %d, code=%" PRId32 "\n", result, ev.code);
        return false;
    }

    /* Peek must leave the event in the queue. */
    result = input__read(&ev, 0);
    if (result != BRUCE_OK || ev.code != BRUCE_INPUT_CODE_DOWN) {
        printf("[selftest] input/peek: FAIL, event missing after peek\n");
        return false;
    }

    printf("[selftest] input/peek: OK\n");
    return true;
}

bool selftest__run_input_wait_case(void) {
    (void)input__flush();

    /* With an empty queue, wait(0) must time out. */
    int32_t code = 0;
    bruce_result_t result = input__wait(0, &code);
    if (result != BRUCE_ERR_TIMEOUT) {
        printf("[selftest] input/wait: FAIL, expected TIMEOUT, got %d\n", result);
        return false;
    }

    /* Inject and wait back. */
    bruce_input_event_t inject = {
        .type = BRUCE_INPUT_KEY,
        .action = BRUCE_INPUT_PRESS,
        .code = 'x',
        .value = 'x',
    };
    result = input__inject(&inject);
    if (result != BRUCE_OK) {
        printf("[selftest] input/wait: FAIL, inject returned %d\n", result);
        return false;
    }

    result = input__wait(100, &code);
    if (result != BRUCE_OK || code != 'x') {
        printf("[selftest] input/wait: FAIL, wait returned %d, code=%" PRId32 "\n", result, code);
        return false;
    }

    printf("[selftest] input/wait: OK\n");
    return true;
}

bool selftest__run_input_check_case(void) {
    (void)input__flush();

    /* Empty queue: check returns false. */
    if (input__check(BRUCE_INPUT_CODE_SELECT, true)) {
        printf("[selftest] input/check: FAIL, check returned true on empty queue\n");
        return false;
    }

    /* Inject two events. */
    bruce_input_event_t inject_a = {
        .type = BRUCE_INPUT_BUTTON,
        .action = BRUCE_INPUT_PRESS,
        .code = BRUCE_INPUT_CODE_UP,
        .value = 1,
    };
    bruce_input_event_t inject_b = {
        .type = BRUCE_INPUT_BUTTON,
        .action = BRUCE_INPUT_PRESS,
        .code = BRUCE_INPUT_CODE_SELECT,
        .value = 1,
    };
    (void)input__inject(&inject_a);
    (void)input__inject(&inject_b);

    /* Non-consuming check for SELECT. */
    if (!input__check(BRUCE_INPUT_CODE_SELECT, false)) {
        printf("[selftest] input/check: FAIL, non-consuming check missed SELECT\n");
        return false;
    }

    /* Consuming check for SELECT should remove it but leave UP. */
    if (!input__check(BRUCE_INPUT_CODE_SELECT, true)) {
        printf("[selftest] input/check: FAIL, consuming check missed SELECT\n");
        return false;
    }

    /* SELECT should now be gone. */
    if (input__check(BRUCE_INPUT_CODE_SELECT, false)) {
        printf("[selftest] input/check: FAIL, SELECT still present after consume\n");
        return false;
    }

    /* UP should still be first in the queue. */
    bruce_input_event_t ev;
    bruce_result_t result = input__read(&ev, 0);
    if (result != BRUCE_OK || ev.code != BRUCE_INPUT_CODE_UP) {
        printf("[selftest] input/check: FAIL, expected UP at head, got %d/%" PRId32 "\n", result, ev.code);
        return false;
    }

    printf("[selftest] input/check: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* input_hotkey.c: chord duration parsing, name<->code lookup, config       */
/* matching, and "emit" action dispatch. Pure logic (no GPIO), so it's      */
/* exercised directly rather than through input_buttons__poll().            */
/* ------------------------------------------------------------------------ */

bool selftest__run_input_hotkey_duration_case(void) {
    struct {
        const char *key;
        uint32_t want_hold_ms;
        const char *want_rest;
    } cases[] = {
        {"2s BTN_B",     2000, "BTN_B"    },
        {"500ms BTN_B",  500,  "BTN_B"    },
        {"BTN_B",        0,    "BTN_B"    },
        {"alt + tab",    0,    "alt + tab"},
        {"2s space",     2000, "space"    },
        {"2s",           0,    "2s"       }, /* no token to bind to: left alone */
        {"",              0,    ""         },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t hold_ms = 0xFFFFFFFF;
        const char *rest = NULL;
        input_hotkey__split_duration(cases[i].key, &hold_ms, &rest);
        if (hold_ms != cases[i].want_hold_ms || rest == NULL || strcmp(rest, cases[i].want_rest) != 0) {
            printf(
                "[selftest] input/hotkey-duration: FAIL, key='%s' got hold_ms=%" PRIu32 " rest='%s'\n",
                cases[i].key,
                hold_ms,
                rest == NULL ? "(null)" : rest
            );
            return false;
        }
    }

    printf("[selftest] input/hotkey-duration: OK\n");
    return true;
}

bool selftest__run_input_hotkey_code_name_case(void) {
    int32_t code = 0;
    bool button_b = input_hotkey__code_for_name("BTN_B", &code) && code == BRUCE_INPUT_CODE_BUTTON_B;
    bool next = input_hotkey__code_for_name("NEXT", &code) && code == BRUCE_INPUT_CODE_NEXT;
    bool prev = input_hotkey__code_for_name("PREV", &code) && code == BRUCE_INPUT_CODE_PREV;
    bool independent = BRUCE_INPUT_CODE_PREV != BRUCE_INPUT_CODE_UP && BRUCE_INPUT_CODE_NEXT != BRUCE_INPUT_CODE_DOWN;
    bool unknown = !input_hotkey__code_for_name("NOT_A_BUTTON", &code) &&
                   !input_hotkey__code_for_name("NAVIGATION_NEXT", &code);

    const char *button_b_name = input_hotkey__name_for_code(BRUCE_INPUT_CODE_BUTTON_B);
    bool reverse_lookup = button_b_name != NULL && strcmp(button_b_name, "BTN_B") == 0;
    const char *prev_name = input_hotkey__name_for_code(BRUCE_INPUT_CODE_PREV);
    bool reverse_prev = prev_name != NULL && strcmp(prev_name, "PREV") == 0;
    bool reverse_unknown = input_hotkey__name_for_code(0x7FFFFFFF) == NULL;

    bool ok = button_b && next && prev && independent && unknown && reverse_lookup && reverse_prev && reverse_unknown;
    printf("[selftest] input/hotkey-code-name: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_input_hotkey_find_case(void) {
    const bruce_config_hotkeys_t *current = config__get_hotkeys();
    if (current == NULL || current->count > BRUCE_CONFIG_HOTKEY_MAX_COUNT) {
        printf("[selftest] input/hotkey-find: FAIL (singleton)\n");
        return false;
    }

    char original_key[BRUCE_CONFIG_HOTKEY_MAX_COUNT][BRUCE_CONFIG_HOTKEY_MAX_LEN + 1] = {0};
    char original_action[BRUCE_CONFIG_HOTKEY_MAX_COUNT][BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1] = {0};
    bruce_config_hotkey_t original[BRUCE_CONFIG_HOTKEY_MAX_COUNT] = {0};
    size_t original_count = current->count;
    for (size_t i = 0; i < original_count; ++i) {
        snprintf(original_key[i], sizeof(original_key[i]), "%s", current->items[i].key);
        snprintf(original_action[i], sizeof(original_action[i]), "%s", current->items[i].action);
        original[i].key = original_key[i];
        original[i].action = original_action[i];
    }

    const bruce_config_hotkey_t test_hotkeys[] = {
        {"BTN_A",       "emit PREV"},
        {"500ms BTN_A", "menu top"},
        {"2s BTN_B",    "menu top"},
        {"BTN_C",       "emit NEXT"},
    };
    bool set_ok = config__set_hotkeys(test_hotkeys, 4) == BRUCE_OK;

    uint32_t hold_ms = 0;
    char action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1] = {0};
    size_t hotkey_index = BRUCE_CONFIG_HOTKEY_MAX_COUNT;
    bool hold_match =
        input_hotkey__find("BTN_B", &hold_ms, action, sizeof(action), &hotkey_index) && hold_ms == 2000 &&
        hotkey_index == 2 &&
        strcmp(action, "menu top") == 0;
    bool instant_match =
        input_hotkey__find("BTN_C", &hold_ms, action, sizeof(action), &hotkey_index) && hold_ms == 0 &&
        hotkey_index == 3 &&
        strcmp(action, "emit NEXT") == 0;
    bool instant_a = input_hotkey__find_by_hold("BTN_A", false, &hold_ms, action, sizeof(action)) &&
                     hold_ms == 0 && strcmp(action, "emit PREV") == 0;
    bool hold_a = input_hotkey__find_by_hold("BTN_A", true, &hold_ms, action, sizeof(action)) &&
                  hold_ms == 500 && strcmp(action, "menu top") == 0;
    bool no_match = !input_hotkey__find("BTN_X", &hold_ms, action, sizeof(action), NULL);

    bool restored = config__set_hotkeys(original, original_count) == BRUCE_OK;

    bool ok = set_ok && hold_match && instant_match && instant_a && hold_a && no_match && restored;
    printf("[selftest] input/hotkey-find: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_input_hotkey_emit_case(void) {
    (void)input__flush();

    input_hotkey__run_action("emit NEXT");

    bruce_input_event_t press;
    bruce_input_event_t release;
    bruce_result_t press_result = input__read(&press, 0);
    bruce_result_t release_result = input__read(&release, 0);
    bool emitted = press_result == BRUCE_OK && press.action == BRUCE_INPUT_PRESS &&
                    press.code == BRUCE_INPUT_CODE_NEXT && release_result == BRUCE_OK &&
                    release.action == BRUCE_INPUT_RELEASE && release.code == BRUCE_INPUT_CODE_NEXT;
    if (!emitted) {
        printf(
            "[selftest] input/hotkey-emit: FAIL, press=%d/%" PRId32 " release=%d/%" PRId32 "\n",
            press_result,
            press.code,
            release_result,
            release.code
        );
        return false;
    }

    /* An unknown "emit" target is recognized-but-inert: no command lookup,
     * no injected event. */
    input_hotkey__run_action("emit NOT_A_REAL_TARGET");
    bruce_input_event_t stray;
    if (input__read(&stray, 0) != BRUCE_ERR_TIMEOUT) {
        printf("[selftest] input/hotkey-emit: FAIL, unknown target injected an event\n");
        return false;
    }

    printf("[selftest] input/hotkey-emit: OK\n");
    return true;
}
