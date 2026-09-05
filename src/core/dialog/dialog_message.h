#pragma once

/* GUI renderers for dialog__message()/dialog__message_show() -- see
 * dialog.c's public API, which dispatches to these when the calling process
 * asked for GUI=1. */

#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

bruce_result_t dialog__gui_message(bruce_dialog_kind_t kind, const char *title, const char *message);

/* Same layout as dialog__gui_message() above, minus the trailing wait for a
 * keypress -- draws once and returns immediately. Meant to sit on screen for
 * whatever blocking call the caller makes right after it, until that
 * caller's own next dialog__* draws something else full-screen over it. */
bruce_result_t dialog__gui_message_show(bruce_dialog_kind_t kind, const char *title, const char *message);
