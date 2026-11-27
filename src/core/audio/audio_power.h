#pragma once

/* Stops the I2S clocks after a few seconds of silence: while enabled, the
 * I2S driver keeps the chip out of light sleep. The next sound restarts them. */
void audio__power_idle(void);
