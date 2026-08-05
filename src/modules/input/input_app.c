#include "input_app.h"

#include "input_buttons.h"
#include "input_common.h"
#include "input_encoder.h"
#include "input_keyboard.h"
#include "input_touch.h"

#include "core_sdk/input.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef INPUT__POLL_PERIOD_MS
#ifdef CONFIG_BRUCE_INPUT_POLL_PERIOD_MS
#define INPUT__POLL_PERIOD_MS CONFIG_BRUCE_INPUT_POLL_PERIOD_MS
#else
#define INPUT__POLL_PERIOD_MS 20
#endif
#endif

uint64_t input__now_ms(void) { return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

void input__emit(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value) {
    bruce_input_event_t event = {
        .type = type,
        .action = action,
        .code = code,
        .value = type == BRUCE_INPUT_KEY && value == 0 ? code : value,
    };
    (void)input__inject(&event);
}

static bruce_result_t input_app__init(void) {
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    input_buttons__init();
    input_keyboard__init();
    input_touch__init();
    input_encoder__init();
#endif
    return BRUCE_OK;
}

static void input_app__poll(void) {
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    input_buttons__poll();
    input_keyboard__poll();
    input_touch__poll();
    input_encoder__poll();
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
