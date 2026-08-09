#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shared hotkey-chord parsing and action dispatch used by both the
 * keyboard (input_keyboard.c) and physical-button (input_buttons.c)
 * pollers.
 *
 * A hotkey `key` string (see core_sdk/config.h's bruce_config_hotkey_t) may
 * carry an optional leading hold-duration token, e.g. "2s BTN_B" or
 * "500ms BTN_B" -- only the button poller acts on it, to fire the action
 * once the button has been held that long instead of on every press.
 * Keyboard chords ("alt + tab") are unaffected: they never carry a
 * duration prefix and keep matching exactly as before.
 *
 * The remainder of the key (after any duration prefix) is a token name.
 * Buttons/navigation codes use the fixed names below; case-sensitive,
 * always uppercase, so they never collide with the lowercase key labels
 * ("a", "tab", ...) keyboard chords are built from:
 *
 *   UP DOWN LEFT RIGHT SELECT BACK MENU HOME DELETE PREV NEXT
 *   BTN_A BTN_B BTN_C BTN_X BTN_Y BTN_L1 BTN_R1 BTN_L2 BTN_R2
 *   BTN_START BTN_SELECT BTN_THUMB_L BTN_THUMB_R
 *
 * A hotkey `action` may be either an AppRunner command line (launched the
 * same way a launcher entry is, e.g. "process preview") or "emit <NAME>",
 * which rebinds the chord to inject a press+release of the named code
 * instead -- e.g. "emit NEXT" emits BRUCE_INPUT_CODE_NEXT.
 */

/* Splits an optional leading "<N>s" / "<N>ms" duration token off `key`.
 * *out_hold_ms is 0 when no duration prefix is present. *out_rest points
 * into `key` (no copy is made) at the token that follows. Always succeeds:
 * anything that doesn't look like a duration prefix is left untouched in
 * *out_rest with *out_hold_ms == 0. */
void input_hotkey__split_duration(const char *key, uint32_t *out_hold_ms, const char **out_rest);

/* Looks up the BRUCE_INPUT_CODE_* value for a token name (e.g. "BTN_B",
 * "UP", "NEXT"). Returns false if the name is not recognized. */
bool input_hotkey__code_for_name(const char *name, int32_t *out_code);

/* Returns the canonical token name for a BRUCE_INPUT_CODE_* value (e.g.
 * BRUCE_INPUT_CODE_BUTTON_B -> "BTN_B"), or NULL if it has none. */
const char *input_hotkey__name_for_code(int32_t code);

/* Finds the configured hotkey whose key, after stripping any duration
 * prefix, equals `name` exactly (first match in configured order wins).
 * On a match, copies its action into out_action, sets *out_hold_ms to that
 * entry's duration prefix (0 for an instant hotkey), and returns true.
 * Returns false if none match or the action does not fit action_size. */
bool input_hotkey__find(const char *name, uint32_t *out_hold_ms, char *out_action, size_t action_size);

/* Runs a matched hotkey action: "emit <NAME>" injects a press+release of
 * the named code; anything else is an AppRunner command line, launched
 * foreground the same way a launcher entry is. No-op for NULL/empty. */
void input_hotkey__run_action(const char *action);
