#include "dialog_gui_common.h"

#include "core/config/config.h"
#include "core_sdk/display.h"

#define DIALOG__TEXT_SIZE 1
#define DIALOG__WIDE_DISPLAY_MIN_WIDTH 200
#define DIALOG__LIST_TEXT_SIZE_WIDE 2

void dialog__get_colors(
    uint16_t *pri, uint16_t *sec, uint16_t *bg, uint16_t *surface, uint16_t *text, uint16_t *text_muted,
    uint16_t *border, uint16_t *success, uint16_t *warning, uint16_t *error
) {
    config__get_colors_internal(pri, sec, bg, surface, text, text_muted, border, success, warning, error);
}

int dialog__default_list_text_size(void) {
    return display__width() >= DIALOG__WIDE_DISPLAY_MIN_WIDTH ? DIALOG__LIST_TEXT_SIZE_WIDE
                                                               : DIALOG__TEXT_SIZE;
}
