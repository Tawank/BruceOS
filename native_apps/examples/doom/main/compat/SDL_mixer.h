// Empty stand-in for SDL_mixer.h. doomgeneric/i_sound.c includes it whenever
// FEATURE_SOUND is defined, but uses nothing from it -- the actual mixing
// lives in the platform sound module (doom_sound.c here). Shadowing the
// header keeps upstream unpatched.
#pragma once
