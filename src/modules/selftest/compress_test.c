#include "compress_test.h"

#include "core_sdk/compress.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char COMPRESS_TEST_SAMPLE[] =
    "the quick brown fox jumps over the lazy dog. the quick brown fox jumps over the lazy dog. "
    "the quick brown fox jumps over the lazy dog. the quick brown fox jumps over the lazy dog.";

static bool compress_test__roundtrip_one_shot(bruce_compress_format_t format, const char *format_name) {
    size_t sample_size = strlen(COMPRESS_TEST_SAMPLE);
    size_t bound = compress__bound(format, sample_size);
    uint8_t compressed[512];
    if (bound > sizeof(compressed)) {
        printf("[selftest] compress/one_shot: %s bound %zu too large for test buffer\n", format_name, bound);
        return false;
    }

    size_t compressed_size = 0;
    bruce_result_t result = compress__compute(
        format, BRUCE_COMPRESS_LEVEL_DEFAULT, COMPRESS_TEST_SAMPLE, sample_size, compressed, sizeof(compressed),
        &compressed_size
    );
    if (result != BRUCE_OK || compressed_size == 0) {
        printf("[selftest] compress/one_shot: %s compress failed (%d)\n", format_name, result);
        return false;
    }

    char decompressed[512];
    size_t decompressed_size = 0;
    result = decompress__compute(
        format, compressed, compressed_size, decompressed, sizeof(decompressed), &decompressed_size
    );
    bool ok = result == BRUCE_OK && decompressed_size == sample_size &&
              memcmp(decompressed, COMPRESS_TEST_SAMPLE, sample_size) == 0;
    if (!ok) {
        printf(
            "[selftest] compress/one_shot: %s decompress mismatch (result=%d size=%zu)\n", format_name, result,
            decompressed_size
        );
    }
    return ok;
}

bool selftest__run_compress_one_shot_case(void) {
    bool ok = compress_test__roundtrip_one_shot(BRUCE_COMPRESS_FORMAT_RAW, "raw") &&
              compress_test__roundtrip_one_shot(BRUCE_COMPRESS_FORMAT_ZLIB, "zlib") &&
              compress_test__roundtrip_one_shot(BRUCE_COMPRESS_FORMAT_GZIP, "gzip");
    printf("[selftest] compress/one_shot: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

/* Feeds/drains a handful of bytes at a time (well under any real chunk size)
 * so the "call again while the output buffer came back completely full"
 * drain loop documented on compress__update()/decompress__update() actually
 * gets exercised, not just the single-call happy path one_shot above covers. */
static bool
compress_test__stream_compress(const char *data, size_t size, uint8_t *out, size_t out_capacity, size_t *out_total) {
    bruce_compress_ctx_t *ctx = compress__start(BRUCE_COMPRESS_FORMAT_GZIP, BRUCE_COMPRESS_LEVEL_DEFAULT);
    if (ctx == NULL) return false;

    static const size_t CHUNK_IN = 16;
    static const size_t CHUNK_OUT = 8;
    size_t total = 0;
    size_t offset = 0;
    bool finished = false;
    bool ok = true;

    while (ok && !finished) {
        size_t this_in = size - offset < CHUNK_IN ? size - offset : CHUNK_IN;
        bool finish = offset + this_in >= size;
        const void *in_ptr = this_in > 0 ? data + offset : NULL;
        offset += this_in;

        bool fed = false;
        for (;;) {
            size_t remaining = out_capacity - total;
            if (remaining == 0) {
                ok = false;
                break;
            }
            size_t this_out_capacity = remaining < CHUNK_OUT ? remaining : CHUNK_OUT;
            size_t written = 0;
            bruce_result_t result = compress__update(
                ctx, fed ? NULL : in_ptr, fed ? 0 : this_in, finish, out + total, this_out_capacity, &written,
                &finished
            );
            fed = true;
            if (result != BRUCE_OK) {
                ok = false;
                break;
            }
            total += written;
            if (finished || written < this_out_capacity) break;
        }
        if (!ok) break;
        if (finish && !finished) {
            ok = false; /* ran out of input on the last chunk without ever finishing */
            break;
        }
    }
    compress__end(ctx);
    if (ok) *out_total = total;
    return ok && finished;
}

static bool compress_test__stream_decompress(
    const uint8_t *data, size_t size, char *out, size_t out_capacity, size_t *out_total
) {
    bruce_compress_ctx_t *ctx = decompress__start(BRUCE_COMPRESS_FORMAT_GZIP);
    if (ctx == NULL) return false;

    static const size_t CHUNK_IN = 8;
    static const size_t CHUNK_OUT = 16;
    size_t total = 0;
    size_t offset = 0;
    bool finished = false;
    bool ok = true;

    while (ok && !finished) {
        size_t this_in = size - offset < CHUNK_IN ? size - offset : CHUNK_IN;
        const void *in_ptr = this_in > 0 ? data + offset : NULL;
        offset += this_in;

        bool fed = false;
        for (;;) {
            size_t remaining = out_capacity - total;
            if (remaining == 0) {
                ok = false;
                break;
            }
            size_t this_out_capacity = remaining < CHUNK_OUT ? remaining : CHUNK_OUT;
            size_t written = 0;
            bruce_result_t result = decompress__update(
                ctx, fed ? NULL : in_ptr, fed ? 0 : this_in, out + total, this_out_capacity, &written, &finished
            );
            fed = true;
            if (result != BRUCE_OK) {
                ok = false;
                break;
            }
            total += written;
            if (finished || written < this_out_capacity) break;
        }
        if (!ok) break;
        if (this_in == 0 && !finished) {
            ok = false; /* ran out of compressed input without ever finishing */
            break;
        }
    }
    decompress__end(ctx);
    if (ok) *out_total = total;
    return ok && finished;
}

bool selftest__run_compress_streaming_case(void) {
    size_t sample_size = strlen(COMPRESS_TEST_SAMPLE);
    uint8_t compressed[512];
    size_t compressed_size = 0;
    bool compress_ok =
        compress_test__stream_compress(COMPRESS_TEST_SAMPLE, sample_size, compressed, sizeof(compressed), &compressed_size);

    char decompressed[512];
    size_t decompressed_size = 0;
    bool decompress_ok =
        compress_ok &&
        compress_test__stream_decompress(compressed, compressed_size, decompressed, sizeof(decompressed), &decompressed_size);

    bool ok = decompress_ok && decompressed_size == sample_size &&
              memcmp(decompressed, COMPRESS_TEST_SAMPLE, sample_size) == 0;
    printf(
        "[selftest] compress/streaming: %s (compressed_size=%zu decompressed_size=%zu)\n", ok ? "OK" : "FAIL",
        compressed_size, decompressed_size
    );
    return ok;
}

bool selftest__run_compress_corrupt_input_case(void) {
    /* No valid gzip magic (0x1f 0x8b) - decompress__update() must report a
     * clean error instead of crashing or claiming success. */
    static const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    char out[64];
    size_t out_size = 0;
    bruce_result_t result =
        decompress__compute(BRUCE_COMPRESS_FORMAT_GZIP, garbage, sizeof(garbage), out, sizeof(out), &out_size);
    bool ok = result != BRUCE_OK;
    printf("[selftest] compress/corrupt_input: %s (result=%d)\n", ok ? "OK" : "FAIL", result);
    return ok;
}

/* Bad-input coverage for compress__compute()/decompress__compute(): every
 * documented failure mode (NULL out-params, a NULL data pointer paired with
 * a non-zero size, an unusable format/level, and an output buffer too small
 * to hold the real decompressed size) must come back as a clean
 * bruce_result_t error - never a crash, and never BRUCE_OK with a bogus
 * *out_size. */
bool selftest__run_compress_invalid_args_case(void) {
    static const char sample[] = "some bytes to compress";
    size_t sample_size = strlen(sample);
    char out[64];
    size_t out_size = 0;

    bool ok = true;

    /* NULL out / out_size. */
    if (compress__compute(BRUCE_COMPRESS_FORMAT_GZIP, BRUCE_COMPRESS_LEVEL_DEFAULT, sample, sample_size, NULL, sizeof(out), &out_size) !=
        BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] compress/invalid_args: NULL out accepted\n");
        ok = false;
    }
    if (compress__compute(BRUCE_COMPRESS_FORMAT_GZIP, BRUCE_COMPRESS_LEVEL_DEFAULT, sample, sample_size, out, sizeof(out), NULL) !=
        BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] compress/invalid_args: NULL out_size accepted\n");
        ok = false;
    }

    /* NULL data with a non-zero size (a NULL data with size == 0 is legal -
     * that's "compress zero bytes"). */
    if (compress__compute(BRUCE_COMPRESS_FORMAT_GZIP, BRUCE_COMPRESS_LEVEL_DEFAULT, NULL, sample_size, out, sizeof(out), &out_size) !=
        BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] compress/invalid_args: NULL data with size>0 accepted\n");
        ok = false;
    }

    /* compress__start() (and so compress__compute()) must reject
     * BRUCE_COMPRESS_FORMAT_AUTO - it's a decompress__start()-only sniffing
     * mode, not a real output container. */
    if (compress__compute(BRUCE_COMPRESS_FORMAT_AUTO, BRUCE_COMPRESS_LEVEL_DEFAULT, sample, sample_size, out, sizeof(out), &out_size) !=
        BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] compress/invalid_args: FORMAT_AUTO accepted for compress\n");
        ok = false;
    }

    /* Levels only run 0-9 (or the BRUCE_COMPRESS_LEVEL_DEFAULT sentinel). */
    if (compress__compute(BRUCE_COMPRESS_FORMAT_GZIP, 42, sample, sample_size, out, sizeof(out), &out_size) !=
        BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] compress/invalid_args: out-of-range level accepted\n");
        ok = false;
    }

    /* decompress__compute() side: NULL out and a too-small out_capacity for
     * a real compressed payload. */
    if (decompress__compute(BRUCE_COMPRESS_FORMAT_GZIP, sample, sample_size, NULL, sizeof(out), &out_size) !=
        BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] compress/invalid_args: NULL decompress out accepted\n");
        ok = false;
    }

    uint8_t compressed[128];
    size_t compressed_size = 0;
    bruce_result_t compress_result = compress__compute(
        BRUCE_COMPRESS_FORMAT_GZIP, BRUCE_COMPRESS_LEVEL_DEFAULT, sample, sample_size, compressed, sizeof(compressed),
        &compressed_size
    );
    char tiny_out[2];
    size_t tiny_out_size = 0;
    bruce_result_t small_buffer_result = compress_result == BRUCE_OK
                                              ? decompress__compute(
                                                    BRUCE_COMPRESS_FORMAT_GZIP, compressed, compressed_size, tiny_out,
                                                    sizeof(tiny_out), &tiny_out_size
                                                )
                                              : compress_result;
    if (compress_result != BRUCE_OK || small_buffer_result != BRUCE_ERR_RESOURCE_LIMIT) {
        printf(
            "[selftest] compress/invalid_args: undersized decompress buffer not rejected (compress_result=%d "
            "small_buffer_result=%d)\n",
            compress_result, small_buffer_result
        );
        ok = false;
    }

    printf("[selftest] compress/invalid_args: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
