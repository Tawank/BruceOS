#include <stdbool.h>
#include <stdint.h>

#include "nes_sound.h"
#include "osd.h"

#if SOUND_ENABLED

#include "core_sdk/audio.h"
#include "core_sdk/runtime.h"

/* nofrendo hands us its APU's fill function (nes.apu->process, see
 * nes_emulate() in nes/nes.c) via osd_setsound() and expects the OSD layer to
 * pull PCM from it periodically -- on most ports that happens from a real
 * sound-card callback; here nes_sound_pump() is called once per video frame
 * from nes_osd.c's/nes_video.c's pace_frame(), the closest thing this port
 * has to a steady clock.
 *
 * The sample count pulled each call is NOT a fixed "one frame's worth"
 * (sample_rate / NES_REFRESH_RATE) -- this emulator does not reliably hit
 * NES_REFRESH_RATE on this hardware, and pace_frame() only paces up to real
 * time, it never slows down for a video frame that ran under budget nor
 * skips ahead for one that ran over. A fixed per-call sample count assumes
 * "one video frame = 1/NES_REFRESH_RATE seconds of real time" -- when a
 * frame actually takes longer than that (CPU+PPU emulation running behind),
 * pulling only the nominal amount underfeeds the I2S DMA ring relative to
 * how much real time actually passed, so the ring drains between calls and
 * the hardware repeats its last queued buffer until fed again: a skipping,
 * "broken record" sound. Instead, samples pulled per call are sized from
 * *actual elapsed wall-clock time* (runtime__now()) since the last pull, so
 * a slow frame naturally pulls proportionally more audio to match, keeping
 * the ring fed at the real rate regardless of how emulation is pacing.
 *
 * apu_process() (nes/sndhrdw/nes_apu.c) has no issue being called with a
 * varying sample count per call: every envelope/sweep/length-counter timer
 * it drives (env_delay, sweep_delay, vbl_length, ...) is a raw sample
 * countdown decremented one sample at a time inside its own mixing loop, not
 * something that assumes a fixed call size -- apu.num_samples (set from this
 * same sample_rate / NES_REFRESH_RATE in apu_setparams()) only sizes the
 * countdown *tables* built once at startup, in units of samples, so it stays
 * correct however the samples arrive.
 *
 * 960 covers the largest single pull this bothers attempting (48kHz for
 * 250ms, the elapsed-time cap below); any additional backlog beyond that is
 * carried forward in audio_carry_frames and paid down over the next calls
 * instead of blowing out this buffer. A backlog large enough to need several
 * calls to pay down (e.g. right after a long stall) doesn't run away or spin
 * the loop hot: audio__stream_write() blocks until the hardware actually
 * accepts each 960-frame chunk, which takes roughly as long as that chunk's
 * real playback time, so each catch-up call still yields to the scheduler
 * for about as long as the audio it just queued. */
#define BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME 960
/* A single gap this large or more (paused in a menu, a very long stall) is
 * treated as "resume from here", not "generate everything that was missed"
 * -- otherwise one long pause would try to catch up an enormous backlog in
 * one go. */
#define BRUCE_NES_AUDIO_MAX_GAP_MS 250u
static int16_t audio_samples[BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME];
static void (*audio_playfunc)(void *buffer, int size);
static uint32_t audio_sample_rate_hz;
static uint64_t audio_last_pump_ms;
static uint32_t audio_carry_frames;
static bool audio_stream_open;

void osd_stopsound(void) {
    audio_playfunc = NULL;
    if (audio_stream_open) {
        audio_stream_open = false;
        audio__stream_close();
    }
    audio_last_pump_ms = 0;
    audio_carry_frames = 0;
}

/* nofrendo calls this once, right before nes_emulate()'s main loop, handing
 * over the APU's own sample-fill function (see nes_emulate() in nes/nes.c).
 * That's also the right moment to open the actual output stream: by now
 * osd_getsoundinfo() has already run (nes_create() calls it to size the
 * APU), so audio__stream_sample_rate() is already the rate the APU was built
 * against. Mono, matching the bps=16/single-channel info reported below --
 * nofrendo's APU mixes all channels down to one before this ever gets
 * called. */
void osd_setsound(void (*playfunc)(void *buffer, int length)) {
    audio_playfunc = playfunc;
    if (playfunc == NULL) {
        osd_stopsound();
        return;
    }
    if (audio_stream_open) return;
    if (audio__stream_open(1) != BRUCE_OK) return;
    audio_stream_open = true;
    audio_sample_rate_hz = audio__stream_sample_rate();
    audio_last_pump_ms = 0;
    audio_carry_frames = 0;
}

void osd_getsoundinfo(sndinfo_t *info) {
    info->sample_rate = (int)audio__stream_sample_rate();
    info->bps = 16;
}

void nes_sound_pump(void) {
    if (!audio_stream_open || audio_playfunc == NULL || audio_sample_rate_hz == 0) return;

    uint64_t now_ms = runtime__now();
    if (audio_last_pump_ms == 0) audio_last_pump_ms = now_ms;
    uint64_t elapsed_ms = now_ms - audio_last_pump_ms;
    if (elapsed_ms > BRUCE_NES_AUDIO_MAX_GAP_MS) elapsed_ms = BRUCE_NES_AUDIO_MAX_GAP_MS;
    audio_last_pump_ms = now_ms;

    /* (elapsed_ms * rate) is a 32-bit-safe multiply for any elapsed window
     * this can produce (250ms cap * 48kHz max rate = 12,000,000, well under
     * UINT32_MAX), and the whole expression is 32-bit throughout -- this
     * emulator's ELF app build has no 64-bit integer divide helpers
     * (__udivdi3/__umoddi3) available in the ELF loader's symbol table (see
     * elf_loader_sdk_symbols.c and the matching comment on pace_frame() in
     * nes_video.c), so this avoids the issue the same way: no operand here
     * is ever a 64-bit value. */
    uint32_t elapsed_frames = (uint32_t)elapsed_ms * audio_sample_rate_hz / 1000u;
    uint32_t due_frames = audio_carry_frames + elapsed_frames;
    uint32_t generate = due_frames > BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME
                            ? BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME
                            : due_frames;
    audio_carry_frames = due_frames - generate;
    if (generate == 0) return;

    audio_playfunc(audio_samples, (int)generate);

    for (uint32_t i = 0; i < generate; i++) { audio_samples[i] = (int16_t)(audio_samples[i] >> 2); }

    audio__stream_write(audio_samples, generate);
}

#else /* !SOUND_ENABLED */

void osd_stopsound(void) {}

/* nofrendo still needs a real, nonzero sample rate here even with audio
 * compiled out: nes_create() (nes/nes.c) feeds osd_getsoundinfo()'s result
 * straight into apu_create(), which divides by it while sizing its
 * envelope/sweep countdown tables. 22050 mirrors the "no audio hardware"
 * fallback the old Arduino port (nes_legacy/nes_sound.c) used for the same
 * reason. */
void osd_getsoundinfo(sndinfo_t *info) {
    info->sample_rate = 22050;
    info->bps = 16;
}

void osd_setsound(void (*playfunc)(void *buffer, int length)) { (void)playfunc; }

void nes_sound_pump(void) {}

#endif /* !SOUND_ENABLED */
