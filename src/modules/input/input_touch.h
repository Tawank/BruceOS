#pragma once

/* Touchscreen (FT5x06/FT6336 family, I2C) - see BRUCE_TOUCH_ENABLED's help
 * text in src/Kconfig.projbuild. No-ops if not enabled. */
void input_touch__init(void);
void input_touch__poll(void);
