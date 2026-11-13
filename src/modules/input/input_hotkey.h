#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/config.h"

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
 * same way a launcher entry is, e.g. "process preview") or "bind <NAME>",
 * which rebinds the key/button to the named code instead, e.g.
 * "bind NEXT". A plain button press mirrors real press/release timing onto
 * the target (input_buttons.c); chords and held actions fire an instant
 * press+release pulse instead (input_hotkey__run_action() below).
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
 * entry's duration prefix (0 for an instant hotkey), optionally stores the
 * configured entry index in out_index, and returns true.
 * Returns false if none match or the action does not fit action_size. */
bool input_hotkey__find(
    const char *name, uint32_t *out_hold_ms, char *out_action, size_t action_size, size_t *out_index
);

typedef struct {
    uint32_t hold_ms; /* 0 for the instant tier */
    char action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1];
} input_hotkey__tier_t;

/* Collects every binding configured for `name` into out_tiers, sorted
 * ascending by hold duration. out_tiers[0] is always the instant
 * (hold_ms == 0) tier, empty action if none configured. Returns the tier
 * count (always >= 1); extra tiers past max_tiers are dropped. */
size_t input_hotkey__find_tiers(const char *name, input_hotkey__tier_t *out_tiers, size_t max_tiers);

/* True iff `action` is "bind <NAME>" with a recognized target; fills *out_code. */
bool input_hotkey__resolve_bind(const char *action, int32_t *out_code);

/* Runs a matched hotkey action: a bind fires a press+release pulse of its
 * target, anything else launches as an AppRunner command. No-op for NULL/empty. */
void input_hotkey__run_action(const char *action);
