#pragma once

/* nofrendo's input glue: translates Bruce SDK key events (core_sdk/input.h)
 * into nofrendo joypad events. Implements osd_getinput()/osd_getmouse(),
 * both declared in osd.h -- nothing else in the app calls into this file
 * directly, so it has no further public surface of its own. Split out from
 * nes_osd.c the same way the old Arduino port kept controller reads in their
 * own nes_controller.cpp, apart from OSD lifecycle glue. */
