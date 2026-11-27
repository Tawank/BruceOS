#pragma once

#include <stdint.h>

/* A mouse as a finger: a pointer on screen, and the left button touching
 * where it points (BRUCE_INPUT_TOUCH press, drag, release), so every app
 * that takes touch takes the mouse. Right button: Back; middle: Select;
 * wheel: Up/Down. Calls come from one task: the device's. */

/* A report: buttons (bit 0 left, 1 right, 2 middle) and relative movement. */
void pointer__report(uint8_t buttons, int dx, int dy, int wheel);
/* Draws the pointer where it went and sends a drag, at most every 30 ms
 * (150 ms on e-paper); hides it after 3 s still. Call every 10-20 ms. */
void pointer__poll(void);
/* The device is gone: lets go of a touch and removes the pointer. */
void pointer__end(void);
