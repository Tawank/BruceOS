#pragma once

/* nofrendo's video driver: renders the emulated NES framebuffer to the Bruce
 * display and paces the emulator to real time every frame. Kept apart from
 * OSD lifecycle glue (nes_osd.c), input (nes_input.c) and audio
 * (nes_sound.c), the same way the old Arduino port split display.cpp out
 * from its osd.c -- what nofrendo itself needs from this file
 * (osd_getvideoinfo(), declared in osd.h) is the only entry point the rest
 * of the app calls directly. */

/* Real hardware has no periodic-timer primitive for nofrendo's
 * osd_installtimer() to arm (see nes_osd.c), so frame pacing instead rides
 * along on video_blit(), the one thing nofrendo already calls once per
 * frame. nes_osd.c's osd_installtimer() hands the callback nofrendo wants
 * ticked here, once, at startup. */
void nes_video_install_timer(void (*callback)(void));

/* Releases the bitmap handed out by the video driver's lock_write callback.
 * nofrendo does not free it itself on shutdown; nes_osd.c's osd_shutdown()
 * calls this as the one place that owns doing so. */
void nes_video_destroy_bitmap(void);
