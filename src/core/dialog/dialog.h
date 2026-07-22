#pragma once

/* Core-private dialog test hooks. Every real dialog__* API is declared
 * exactly once in "core_sdk/dialog.h"; this header must never redeclare any
 * of it. It exists solely so modules/selftest (the one built-in exempted
 * from the core_sdk-only-headers rule) can exercise dialog__choice()
 * without blocking on real console I/O and can verify GUI/terminal renderer
 * dispatch. No other Core service or built-in should include this header. */

#include <stddef.h>

#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

typedef bruce_result_t (*dialog__test_choice_provider_t)(const char *title, const char *message,
                                                          const bruce_dialog_choice_t *choices, size_t choice_count,
                                                          size_t *out_selected);

/* Overrides dialog__choice()'s rendering with `provider` (called instead of
 * the real console prompt); pass NULL to restore normal behavior. */
void dialog__test_set_choice_provider(dialog__test_choice_provider_t provider);

/* Returns whether the most recent dialog__message()/dialog__choice() call
 * selected the GUI renderer (the calling task's --gui context) rather than
 * the terminal one. */
bool dialog__test_last_call_was_gui(void);
