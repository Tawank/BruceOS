#pragma once

#include <stdbool.h>

bool selftest__run_audio_validation_case(void);
bool selftest__run_audio_stream_nonblocking_case(void);
bool selftest__run_audio_kill_mid_tone_case(void);
bool selftest__run_audio_mixer_concurrent_case(void);
bool selftest__run_audio_mixer_tone_over_stream_case(void);
bool selftest__run_audio_mixer_kill_frees_slot_case(void);
bool selftest__run_audio_file_wav_leak_case(void);
bool selftest__run_audio_file_mp3_leak_case(void);
