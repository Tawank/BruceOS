#include <stdbool.h>
#include <stdint.h>

#include "nes_sound.h"
#include "osd.h"

#if SOUND_ENABLED

#include "core_sdk/audio.h"
#include "nes/nes.h"

/* nofrendo supplies an APU fill function and expects this port to pull PCM
 * once per emulated frame. The non-blocking Core stream drops excess PCM
 * under load instead of allowing audio backpressure to delay emulation. */
#define BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME 960
static int16_t audio_samples[BRUCE_NES_AUDIO_MAX_SAMPLES_PER_FRAME];
static void (*audio_playfunc)(void *buffer, int size);
static uint32_t audio_sample_rate_hz;
static uint32_t audio_frame_fraction;
static bool audio_stream_open;

/* Retained for the video performance report; submission never blocks. */
static uint32_t audio_last_write_us;

uint32_t nes_sound_last_write_us(void) { return audio_last_write_us; }

void osd_stopsound(void) {
    audio_playfunc = NULL;
    if (audio_stream_open) {
        audio_stream_open = false;
        audio__stream_close();
    }
    audio_frame_fraction = 0;
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
    audio_frame_fraction = 0;
}

void osd_getsoundinfo(sndinfo_t *info) {
    info->sample_rate = (int)audio__stream_sample_rate();
    info->bps = 16;
}

void nes_sound_pump(void) {
    if (!audio_stream_open || audio_playfunc == NULL || audio_sample_rate_hz == 0) return;

    /* The APU advances in emulated time, not wall-clock time. Produce one
     * fractional NES frame of PCM and drop it if the stream FIFO is full. */
    audio_frame_fraction += audio_sample_rate_hz;
    uint32_t generate = audio_frame_fraction / NES_REFRESH_RATE;
    audio_frame_fraction %= NES_REFRESH_RATE;

    audio_playfunc(audio_samples, (int)generate);

    for (uint32_t i = 0; i < generate; i++) { audio_samples[i] = (int16_t)(audio_samples[i] >> 2); }

    (void)audio__stream_write(audio_samples, generate);
    audio_last_write_us = 0;
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
