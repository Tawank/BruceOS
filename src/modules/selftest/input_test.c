#include "input_test.h"

#include <inttypes.h>
#include <stdio.h>

#include "core_sdk/input.h"
#include "core_sdk/result.h"

bool selftest__run_input_poll_case(void)
{
    bruce_input_event_t ev;
    bruce_result_t result = input__poll(&ev);
    if (result == BRUCE_ERR_TIMEOUT) {
        printf("[selftest] input/poll: OK (empty queue returns TIMEOUT)\n");
        return true;
    }
    printf("[selftest] input/poll: FAIL, expected TIMEOUT, got %d\n", result);
    return false;
}

bool selftest__run_input_inject_case(void)
{
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

    if (ev.type != BRUCE_INPUT_BUTTON || ev.action != BRUCE_INPUT_PRESS || ev.code != BRUCE_INPUT_CODE_SELECT ||
        ev.value != 1) {
        printf("[selftest] input/inject: FAIL, event mismatch type=%d action=%d code=%" PRId32 " value=%" PRId32 "\n",
               ev.type, ev.action, ev.code, ev.value);
        return false;
    }

    printf("[selftest] input/inject: OK\n");
    return true;
}

bool selftest__run_input_flush_case(void)
{
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

bool selftest__run_input_non_blocking_case(void)
{
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
        printf("[selftest] input/nonblocking: FAIL, read returned %d, code=%" PRId32 " value=%" PRId32 "\n", result,
               ev.code, ev.value);
        return false;
    }

    printf("[selftest] input/nonblocking: OK\n");
    return true;
}

bool selftest__run_input_peek_case(void)
{
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

bool selftest__run_input_wait_case(void)
{
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

bool selftest__run_input_check_case(void)
{
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
