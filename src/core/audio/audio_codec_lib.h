#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_audio_dec_reg.h"

/* What lib_main() of an audio codec library in /lib returns (see core/lib):
 * the decoders it carries, which Core registers with esp_audio_dec_register()
 * while a file needs them. Built by tools/build_libs.py. */
#define AUDIO_CODEC_LIB_ABI 1
#define AUDIO_CODEC_LIB_DECODERS_MAX 2

typedef struct {
    esp_audio_type_t type;
    esp_audio_dec_ops_t ops;
} audio_codec_lib_decoder_t;

typedef struct {
    uint32_t abi;
    /* esp_audio_codec's version the library was built from; it must match
     * the firmware's, since the decoders share its structs. */
    const char *(*codec_version)(void);
    size_t decoder_count;
    audio_codec_lib_decoder_t decoders[AUDIO_CODEC_LIB_DECODERS_MAX];
} audio_codec_lib_t;
