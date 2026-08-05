#pragma once

/* Cardputer-style 3-output/7-input GPIO scan matrix keyboard (see
 * src/Kconfig.projbuild's BRUCE_KEYBOARD_ENABLED help text). No-ops if not
 * enabled. */
void input_keyboard__init(void);
void input_keyboard__poll(void);
