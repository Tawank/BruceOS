#pragma once

/* Quadrature rotary encoder (e.g. M5Stack DinMeter), decoded in software and
 * synthesized as UP/DOWN presses - see BRUCE_ENCODER_ENABLED's help text in
 * src/Kconfig.projbuild. No-ops if not enabled. */
void input_encoder__init(void);
void input_encoder__poll(void);
