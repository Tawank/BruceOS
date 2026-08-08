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
 * This cap was briefly raised to 1440 (the full I2S DMA ring depth, see
 * AUDIO__I2S_DMA_RING_FRAMES in core/audio/audio.c) on the theory that a
 * bigger cap lets one call fully drain any backlog instead of dribbling
 * behind. Measurement (nes_video.c's perf report) on real hardware showed
 * that backfired: once render+CPU/PPU-emulation alone are slower than the
 * cap's worth of real time -- true here even after most frames' *blit* is
 * skipped, see nes_video.c's skip_track_debt() -- `due_frames` sits above
 * the cap on essentially every call, so `generate` saturates at the cap
 * every single time. At that point this is indistinguishable from "always
 * write exactly cap-sized chunks", and because audio__stream_write() blocks
 * until the hardware has room (it paces to real playback speed once the
 * ring can't absorb a write immediately), a saturated loop settles into
 * spending very close to (cap / sample_rate) seconds *blocked inside this
 * call* every iteration -- the cap becomes a floor on iteration time, not
 * just a ceiling on catch-up. Raising it from 960 (20ms) to 1440 (30ms)
 * measured out to raising that floor from ~20ms to ~30ms, i.e. made the
 * game slower, not faster -- the opposite of the intent. Keeping the cap
 * close to *one nominal NES frame's worth* (audio_sample_rate_hz /
 * NES_REFRESH_RATE, ~800 for NTSC @ 48kHz) bounds that floor close to the
 * frame budget itself; 960 (1.2x nominal) leaves a little slack for jitter
 * without letting a chronic backlog turn into a chronic ~2x slowdown.
 * The tradeoff this leaves in place: whenever CPU/PPU emulation + blit
 * genuinely can't keep up with real time on their own (a real, separate
 * bottleneck -- see nes_video.c's perf report for whether that's the case
 * here), the ring still can't be kept fully fed and will underrun/glitch.
 * That's a real limit of pulling audio from a single task that also does
 * the emulation and blocking on a real-time output device; it can't be
 * fixed by turning the knob in this file, only masked at the cost of
 * exactly the resonant slowdown above. Any backlog beyond what the cap
 * covers in one call is carried forward in audio_carry_frames and paid down
 * over subsequent calls instead of blowing out the audio_samples buffer. */
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

/* Wall-clock time the most recent nes_sound_pump() call spent blocked
 * inside audio__stream_write(), in microseconds -- exposed so nes_video.c's
 * perf report can show it alongside blit/iteration time, telling apart "the
 * loop is slow because emulation is slow" from "the loop is slow because
 * it's stuck waiting on the audio backend" (see the cap-saturation comment
 * above BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME). */
static uint32_t audio_last_write_us;

uint32_t nes_sound_last_write_us(void) { return audio_last_write_us; }

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

    uint64_t write_start_ms = runtime__now();
    audio__stream_write(audio_samples, generate);
    audio_last_write_us = (uint32_t)(runtime__now() - write_start_ms) * 1000u;
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

uint32_t nes_sound_last_write_us(void) { return 0; }

#endif /* !SOUND_ENABLED */
