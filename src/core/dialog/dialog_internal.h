#pragma once

/* Shared between dialog.c (owns the GUI/terminal renderer decision and the
 * dialog__test_last_call_was_gui() test hook) and any other core/dialog
 * dialog_*.c file whose own public API entry points need that same
 * decision -- currently just dialog_viewer.c. Every other split file
 * (dialog_term.c, dialog_message.c, dialog_choice.c, dialog_pick_file.c)
 * only implements the GUI or terminal renderer itself; dialog.c's own
 * public API wrappers are what call dialog__current_process_wants_gui()
 * and record the result, so they don't need this header. */

#include <stdbool.h>

/* True when the calling process wants GUI dialogs (its GUI=1 context). */
bool dialog__current_process_wants_gui(void);

/* Records `gui` for dialog__test_last_call_was_gui() to read back. */
void dialog__note_last_call_was_gui(bool gui);
