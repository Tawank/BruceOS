// Bruce platform backend for doomgeneric's sound hooks (DG_sound_module /
// DG_music_module -- see doomgeneric/i_sound.h). Compiled with FEATURE_SOUND
// defined (see CMakeLists.txt), which makes i_sound.c route every
// I_StartSound()/I_UpdateSound()/... call through the module below instead of
// silently no-oping.
//
// Sound effects only. Doom's music is MUS data meant for an OPL2 FM synth;
// doomgeneric ships no synthesizer, and emulating one alongside the software
// renderer is a separate (much heavier) job -- DG_music_module is a stub.
//
// Everything here runs on Doom's own game thread: s_sound.c calls
// StartSound/StopSound/UpdateSoundParams from game logic, and Update() once
// per doomgeneric_Tick() (via S_UpdateSounds()). Update() mixes all active
// channels into a mono int16 buffer and pushes as much as the Core stream
// FIFO will take right now (audio__stream_writable_frames()), so no locking
// or extra task is needed. Keeping the FIFO topped up -- rather than writing
// exactly one tic's worth -- is what lets playback survive frames that take
// longer than a tic; the cost is up to one FIFO's worth of latency.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/audio.h"
#include "core_sdk/result.h"

#include "deh_str.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

// m_config.c / i_sound.c bind these as config variables whenever
// FEATURE_SOUND is defined; upstream defines them in the SDL/Allegro sound
// backends, which this port doesn't build. Unused here.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// dummy.c only provides this when FEATURE_SOUND is *not* defined.
void I_InitTimidityConfig(void) {}

// s_sound.c's snd_channels defaults to 8; a little headroom costs nothing.
#define DOOM_SOUND_CHANNELS 16
// Mixed per audio__stream_write() call. Update() loops until the FIFO is
// full, so this only bounds the stack/static buffer, not throughput.
#define DOOM_SOUND_MIX_FRAMES 512

typedef struct {
    const uint8_t *data; // 8-bit unsigned PCM, header and DMX padding skipped
    uint32_t length;     // samples
    uint32_t pos;        // 16.16 fixed-point position into data
    uint32_t step;       // 16.16 fixed-point increment per output frame
    int lumpnum;
    int vol;             // 0..127
    boolean active;
} doom_sound_channel_t;

static doom_sound_channel_t s_channels[DOOM_SOUND_CHANNELS];
static int32_t s_mix_accum[DOOM_SOUND_MIX_FRAMES];
static int16_t s_mix_buffer[DOOM_SOUND_MIX_FRAMES];
static uint32_t s_output_rate;
static boolean s_use_sfx_prefix;
static boolean s_stream_open;

// Lumps are cached PU_STATIC while any channel plays them, then dropped back
// to PU_CACHE so Doom's zone allocator may purge them. W_ReleaseLumpNum()
// isn't reference counted, so only release once no other channel still
// points at the same lump.
static void ReleaseLumpIfUnused(int lumpnum) {
    for (int i = 0; i < DOOM_SOUND_CHANNELS; i++) {
        if (s_channels[i].active && s_channels[i].lumpnum == lumpnum) return;
    }
    W_ReleaseLumpNum(lumpnum);
}

static void ReleaseChannel(doom_sound_channel_t *ch) {
    if (!ch->active) return;
    ch->active = false;
    ReleaseLumpIfUnused(ch->lumpnum);
}

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t buf_len) {
    if (sfx->link != NULL) sfx = sfx->link;
    if (s_use_sfx_prefix) {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    } else {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static snddevice_t sound_bruce_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

static boolean Bruce_Init(boolean use_sfx_prefix) {
    s_use_sfx_prefix = use_sfx_prefix;
    memset(s_channels, 0, sizeof(s_channels));
    if (audio__stream_open(1) != BRUCE_OK) {
        printf("doom: audio stream unavailable, sound disabled\n");
        return false;
    }
    s_stream_open = true;
    s_output_rate = audio__stream_sample_rate();
    printf("doom: sound ready (%u Hz mono)\n", (unsigned)s_output_rate);
    return true;
}

static void Bruce_Shutdown(void) {
    for (int i = 0; i < DOOM_SOUND_CHANNELS; i++) ReleaseChannel(&s_channels[i]);
    if (s_stream_open) {
        audio__stream_close();
        s_stream_open = false;
    }
}

// W_CheckNumForName rather than W_GetNumForName: a PWAD/IWAD missing one
// sound shouldn't be a fatal I_Error(). StartSound() ignores lumpnum -1.
static int Bruce_GetSfxLumpNum(sfxinfo_t *sfx) {
    char namebuf[9];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_CheckNumForName(namebuf);
}

static void Bruce_Update(void) {
    if (!s_stream_open) return;

    size_t writable = audio__stream_writable_frames();
    while (writable > 0) {
        size_t frames = writable < DOOM_SOUND_MIX_FRAMES ? writable : DOOM_SOUND_MIX_FRAMES;
        int32_t *mix = s_mix_accum;
        memset(mix, 0, frames * sizeof(mix[0]));

        for (int c = 0; c < DOOM_SOUND_CHANNELS; c++) {
            doom_sound_channel_t *ch = &s_channels[c];
            if (!ch->active) continue;
            for (size_t i = 0; i < frames; i++) {
                uint32_t idx = ch->pos >> 16;
                if (idx >= ch->length) {
                    ReleaseChannel(ch);
                    break;
                }
                // u8 -> signed, scaled to roughly int16 range, then volume.
                mix[i] += (((int32_t)ch->data[idx] - 128) * ch->vol) << 1;
                ch->pos += ch->step;
            }
        }

        for (size_t i = 0; i < frames; i++) {
            int32_t s = mix[i];
            if (s > INT16_MAX) s = INT16_MAX;
            if (s < INT16_MIN) s = INT16_MIN;
            s_mix_buffer[i] = (int16_t)s;
        }

        size_t written = audio__stream_write(s_mix_buffer, frames);
        if (written < frames) break;
        writable -= frames;
    }
}

// Output is mono, so separation (stereo pan) is ignored.
static void Bruce_UpdateSoundParams(int channel, int vol, int sep) {
    (void)sep;
    if (channel < 0 || channel >= DOOM_SOUND_CHANNELS) return;
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    s_channels[channel].vol = vol;
}

static int Bruce_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (!s_stream_open || channel < 0 || channel >= DOOM_SOUND_CHANNELS) return -1;

    doom_sound_channel_t *ch = &s_channels[channel];
    ReleaseChannel(ch);

    int lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0) return -1;

    const uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC);
    uint32_t lumplen = (uint32_t)W_LumpLength(lumpnum);

    // DMX sound lump: u16 format (3), u16 sample rate, u32 sample count,
    // then 8-bit unsigned PCM. Same validation as chocolate-doom's
    // i_sdlsound.c CacheSFX(), including DMX's habit of skipping the first
    // and last 16 samples.
    uint32_t rate = 0, length = 0;
    if (lumplen >= 8) {
        rate = data[2] | (data[3] << 8);
        length = data[4] | (data[5] << 8) | (data[6] << 16) | ((uint32_t)data[7] << 24);
    }
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00 || length > lumplen - 8 || length <= 48 || rate == 0) {
        ReleaseLumpIfUnused(lumpnum);
        return -1;
    }

    ch->data = data + 8 + 16;
    ch->length = length - 32;
    ch->pos = 0;
    ch->step = (uint32_t)(((uint64_t)rate << 16) / s_output_rate);
    ch->lumpnum = lumpnum;
    ch->active = true;
    Bruce_UpdateSoundParams(channel, vol, sep);
    return channel;
}

static void Bruce_StopSound(int channel) {
    if (channel < 0 || channel >= DOOM_SOUND_CHANNELS) return;
    ReleaseChannel(&s_channels[channel]);
}

static boolean Bruce_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= DOOM_SOUND_CHANNELS) return false;
    return s_channels[channel].active;
}

// Sounds are loaded lazily in StartSound(); precaching every lump up front
// would pin all of them in the zone for the whole session.
static void Bruce_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds;
    (void)num_sounds;
}

sound_module_t DG_sound_module = {
    sound_bruce_devices,
    arrlen(sound_bruce_devices),
    Bruce_Init,
    Bruce_Shutdown,
    Bruce_GetSfxLumpNum,
    Bruce_Update,
    Bruce_UpdateSoundParams,
    Bruce_StartSound,
    Bruce_StopSound,
    Bruce_SoundIsPlaying,
    Bruce_CacheSounds,
};

// Music: not implemented (see top of file). Every hook is a no-op so
// i_sound.c's unconditional music_module calls are safe.
static boolean Music_Init(void) { return true; }
static void Music_Shutdown(void) {}
static void Music_SetVolume(int volume) { (void)volume; }
static void Music_Pause(void) {}
static void Music_Resume(void) {}
static void *Music_RegisterSong(void *data, int len) {
    (void)data;
    (void)len;
    return NULL;
}
static void Music_UnRegisterSong(void *handle) { (void)handle; }
static void Music_PlaySong(void *handle, boolean looping) {
    (void)handle;
    (void)looping;
}
static void Music_StopSong(void) {}
static boolean Music_IsPlaying(void) { return false; }

music_module_t DG_music_module = {
    NULL,
    0,
    Music_Init,
    Music_Shutdown,
    Music_SetVolume,
    Music_Pause,
    Music_Resume,
    Music_RegisterSong,
    Music_UnRegisterSong,
    Music_PlaySong,
    Music_StopSong,
    Music_IsPlaying,
    NULL,
};
