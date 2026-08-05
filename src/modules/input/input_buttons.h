#pragma once

/* GPIO buttons: SELECT plus up to four discrete A/B/C/D buttons (see
 * src/Kconfig.projbuild's Input menu). No-ops if none are enabled. */
void input_buttons__init(void);
void input_buttons__poll(void);
