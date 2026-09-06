#include "ir_tvbgone.h"

#include <stdint.h>
#include <stdio.h>

#include "core_sdk/dialog.h"
#include "core_sdk/input.h"
#include "core_sdk/ir.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"

#include "WORLD_IR_CODES.h"

static bruce_result_t ir_tvbgone__show_progress(size_t sent, size_t total, bruce_viewer_id_t viewer) {
    if (viewer == BRUCE_VIEWER_ID_INVALID) return BRUCE_OK;
    char progress[96];
    snprintf(
        progress,
        sizeof(progress),
        "Sending power codes\n%u / %u\n\nBack: stop",
        (unsigned int)sent,
        (unsigned int)total
    );
    return dialog__viewer_set_text(viewer, progress);
}

static bruce_result_t ir_tvbgone__send_code(const IrCode *code, uint32_t *data) {
    if (code == NULL || code->numpairs == 0 || (size_t)code->numpairs * 2u > BRUCE_IR_MAX_RAW_TIMINGS) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    size_t bit_offset = 0;
    for (size_t pair = 0; pair < code->numpairs; ++pair) {
        uint8_t index = 0;
        for (uint8_t bit = 0; bit < code->bitcompression; ++bit) {
            index =
                (uint8_t)((index << 1u) | ((code->codes[bit_offset / 8u] >> (7u - bit_offset % 8u)) & 1u));
            bit_offset++;
        }
        data[pair * 2u] = (uint32_t)code->times[index * 2u] * 10u;
        data[pair * 2u + 1u] = (uint32_t)code->times[index * 2u + 1u] * 10u;
    }
    return ir__transmit_raw(data, (size_t)code->numpairs * 2u, (uint32_t)code->timer_val * 1000u, 0);
}

static bruce_result_t ir_tvbgone__send_raw_code(const RawIrCode *code) {
    if (code == NULL || code->numpairs == 0 || (size_t)code->numpairs * 2u > BRUCE_IR_MAX_RAW_TIMINGS) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return ir__transmit_raw(code->times, (size_t)code->numpairs * 2u, (uint32_t)code->timer_val * 1000u, 0);
}

bruce_result_t ir_tvbgone__run(bool europe, bool gui) {
    const IrCode *const *regional = europe ? EUpowerCodes : NApowerCodes;
    size_t regional_count = europe ? num_EUcodes : num_NAcodes;
    size_t total = regional_count + num_UniversalParsedCodes + num_UniversalRawCodes;
    size_t sent = 0;
    uint32_t *data = memory__malloc(BRUCE_IR_MAX_RAW_TIMINGS * sizeof(*data));
    if (data == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    if (gui) {
        (void)input__flush();
        (void)dialog__create_text_viewer("TV-B-Gone", "Starting...\n\nBack: stop", &viewer);
    }
    bruce_result_t result = BRUCE_OK;
    for (size_t i = 0; result == BRUCE_OK && i < regional_count; ++i) {
        if (input__check(BRUCE_INPUT_CODE_BACK, true) || input__check(BRUCE_INPUT_CODE_BUTTON_B, true)) {
            result = BRUCE_ERR_CANCELLED;
            break;
        }
        result = ir_tvbgone__send_code(regional[i], data);
        if (result == BRUCE_OK) result = ir_tvbgone__show_progress(++sent, total, viewer);
        if (result == BRUCE_OK && runtime__delay(205) != BRUCE_OK) result = BRUCE_ERR_CANCELLED;
    }
    for (size_t i = 0; result == BRUCE_OK && i < num_UniversalParsedCodes; ++i) {
        if (input__check(BRUCE_INPUT_CODE_BACK, true) || input__check(BRUCE_INPUT_CODE_BUTTON_B, true)) {
            result = BRUCE_ERR_CANCELLED;
            break;
        }
        result = ir_tvbgone__send_code(UniversalParsedCodes[i], data);
        if (result == BRUCE_OK) result = ir_tvbgone__show_progress(++sent, total, viewer);
        if (result == BRUCE_OK && runtime__delay(205) != BRUCE_OK) result = BRUCE_ERR_CANCELLED;
    }
    for (size_t i = 0; result == BRUCE_OK && i < num_UniversalRawCodes; ++i) {
        if (input__check(BRUCE_INPUT_CODE_BACK, true) || input__check(BRUCE_INPUT_CODE_BUTTON_B, true)) {
            result = BRUCE_ERR_CANCELLED;
            break;
        }
        result = ir_tvbgone__send_raw_code(UniversalRawCodes[i]);
        if (result == BRUCE_OK) result = ir_tvbgone__show_progress(++sent, total, viewer);
        if (result == BRUCE_OK && runtime__delay(205) != BRUCE_OK) result = BRUCE_ERR_CANCELLED;
    }
    if (viewer != BRUCE_VIEWER_ID_INVALID) (void)dialog__viewer_close(viewer);
    if (gui)
        (void)dialog__message(
            result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_WARNING,
            "TV-B-Gone",
            result == BRUCE_OK ? "All codes sent" : "Stopped"
        );
    memory__free(data);
    return result;
}
