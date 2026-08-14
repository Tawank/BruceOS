#pragma once

#include <stdint.h>

/* NES audio: implements nofrendo's OSD sound interface (osd_setsound(),
 * osd_getsoundinfo(), see osd.h) plus osd_stopsound() -- not part of osd.h's
 * declared interface, but nofrendo's own convention for the function that
 * undoes osd_setsound(), same as every other nofrendo port -- on top of the
 * Bruce SDK's audio__stream_*() API (core_sdk/audio.h). All of it lives in
 * nes_sound.c; this header only exposes what the rest of the app needs to
 * drive it from outside. */

/* Flip to 0 to build the app with audio compiled out entirely. nes_sound.c
 * still provides every symbol nofrendo and the rest of this app call
 * (osd_setsound(), osd_getsoundinfo(), osd_stopsound(), nes_sound_pump()) --
 * disabled mode just makes them all no-ops (osd_getsoundinfo() still reports
 * a nominal sample rate; nofrendo's apu_create() divides by it while sizing
 * lookup tables regardless of whether anything plays), so no audio stream is
 * ever opened and nofrendo's APU is never pulled from. That second part also
 * makes this the first thing to flip off for a speed check: the APU's own
 * per-sample envelope/sweep/length-counter work only happens inside the
 * osd_setsound() fill callback, so skipping it here skips that CPU cost too,
 * not just the I2S write. */
#define SOUND_ENABLED 1

/* Pulls one emulated frame's PCM from nofrendo's APU and submits it to the
 * non-blocking Bruce audio stream. Call once per video frame; a no-op if no
 * stream is open or whenever SOUND_ENABLED is 0. */
void nes_sound_pump(void);

/* Closes the audio stream opened by osd_setsound(), if any. Not part of
 * nofrendo's osd.h interface (nofrendo never calls it itself), but needed
 * from nes_osd.c's osd_shutdown() as the one in-band chance to close the
 * stream before the process exits. */
void osd_stopsound(void);

/* Always zero with the non-blocking audio stream. Retained for the video
 * performance report's stable interface. */
uint32_t nes_sound_last_write_us(void);
