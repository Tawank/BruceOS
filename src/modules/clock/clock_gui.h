#pragma once

#include "clock_screens.h"

/* Runs the clock GUI until the user closes it; timers, alarms and the stopwatch keep running while minimised. */
int clock_gui__run(clock_screen_t first_screen);
