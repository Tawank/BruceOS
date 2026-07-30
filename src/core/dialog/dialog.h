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

/* `out_buffer` is a caller-owned buffer of `buffer_size` bytes; the provider
 * fills the final string and returns BRUCE_OK or BRUCE_ERR_CANCELLED. */
typedef bruce_result_t (*dialog__test_input_provider_t)(const char *title, const char *prompt,
                                                          const char *initial_text, bool mask_input,
                                                          char *out_buffer, size_t buffer_size);

/* `out_path` is a caller-owned buffer of `out_path_size` bytes. */
typedef bruce_result_t (*dialog__test_pick_file_provider_t)(const char *initial_path,
                                                              const char *extension_filter,
                                                              char *out_path, size_t out_path_size);

/* Overrides dialog__choice()'s rendering with `provider` (called instead of
 * the real console prompt); pass NULL to restore normal behavior. */
void dialog__test_set_choice_provider(dialog__test_choice_provider_t provider);

/* Overrides text/hex/number input dialogs.  The provider is invoked with the
 * same arguments as the public dialog__*input() API and must fill `out_buffer`. */
void dialog__test_set_input_provider(dialog__test_input_provider_t provider);

/* Overrides dialog__pick_file(). */
void dialog__test_set_pick_file_provider(dialog__test_pick_file_provider_t provider);

/* Returns whether the most recent dialog__* call selected the GUI renderer
 * (the calling process's --gui context) rather than the terminal one. */
bool dialog__test_last_call_was_gui(void);
