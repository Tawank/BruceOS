#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief DEFLATE compression/decompression (zlib-backed).
 *
 * Used to compress/decompress sprites, bitmaps, and general file data, and
 * as the codec layer under core_sdk/archive.h's ".tar.gz" support. No
 * permission of its own - this only ever touches memory the caller already
 * gave it; a caller doing file I/O around it (e.g. writing the compressed
 * output to storage) needs whatever permission that I/O itself requires.
 *
 * Two ways to use it:
 *
 *   - The one-shot compress__compute()/decompress__compute() functions, for
 *     a buffer that's already fully in memory and whose *decompressed* size
 *     is already known ahead of time (e.g. a sprite's own header already
 *     states its uncompressed width*height*bpp) - the common case for
 *     sprites/bitmaps.
 *   - The streaming compress__start()/compress__update()/compress__end() and
 *     decompress__start()/decompress__update()/decompress__end() functions,
 *     for arbitrary-size data (a whole file, an archive member) that
 *     shouldn't ever need to sit fully in RAM at once. This is a thin,
 *     honest wrapper over zlib's own deflate()/inflate() call contract
 *     rather than one that hides it - compressed output size isn't bounded
 *     by input chunk size, so any API that pretended otherwise would either
 *     lie about completion or require an unbounded output buffer.
 */

typedef enum {
    /* RFC1951 raw deflate: no header, no trailer, no checksum. Smallest
     * output, for embedding inside a caller's own container that already
     * tracks the original size (e.g. a sprite file's header). */
    BRUCE_COMPRESS_FORMAT_RAW = 0,
    /* RFC1950 zlib wrapper: 2-byte header + Adler-32 trailer. */
    BRUCE_COMPRESS_FORMAT_ZLIB,
    /* RFC1952 gzip container: magic/OS-byte header + CRC-32/size trailer -
     * what ".gz" files and the gzip layer of ".tar.gz" use. */
    BRUCE_COMPRESS_FORMAT_GZIP,
    /* decompress__start() only: sniff zlib-vs-gzip from the stream's own
     * header instead of the caller having to know which one it is. */
    BRUCE_COMPRESS_FORMAT_AUTO,
} bruce_compress_format_t;

/** zlib's own compression-level range, or BRUCE_COMPRESS_LEVEL_DEFAULT for its usual balance of speed vs ratio. */
#define BRUCE_COMPRESS_LEVEL_DEFAULT (-1)
#define BRUCE_COMPRESS_LEVEL_FASTEST 1
#define BRUCE_COMPRESS_LEVEL_BEST 9

typedef struct bruce_compress_ctx bruce_compress_ctx_t;

/**
 * @brief Upper bound on compress__compute()'s output size for `input_size`
 * bytes of input, to size `out` with.
 *
 * Compression can (rarely, for already-dense input) make data slightly
 * larger; this is the worst case, not the expected case.
 */
size_t compress__bound(bruce_compress_format_t format, size_t input_size);

/**
 * @brief Compresses a buffer already fully in memory, in one call.
 *
 * Equivalent to compress__start() + compress__update() (with finish=true) +
 * compress__end(), for the common case where `data` is small enough to
 * already be fully in RAM. Prefer the streaming API for anything
 * file-sized.
 *
 * @param format Output container. Must not be BRUCE_COMPRESS_FORMAT_AUTO.
 * @param level BRUCE_COMPRESS_LEVEL_DEFAULT, or 0-9 (0 = store, 9 = smallest/slowest).
 * @param data Bytes to compress.
 * @param size Number of bytes in data.
 * @param out Buffer to receive the compressed output.
 * @param out_capacity Size of out; compress__bound(format, size) is always enough.
 * @param out_size Receives the number of bytes written to out.
 */
bruce_result_t compress__compute(
    bruce_compress_format_t format, int level, const void *data, size_t size, void *out, size_t out_capacity,
    size_t *out_size
);

/**
 * @brief Decompresses a buffer already fully in memory, in one call.
 *
 * The decompressed size must already be known and `out_capacity` must be at
 * least that large - this never grows `out` or guesses; it fails with
 * BRUCE_ERR_RESOURCE_LIMIT if the decompressed data doesn't fit. Prefer the
 * streaming API when the decompressed size isn't known ahead of time (the
 * general "decompress a file" case).
 *
 * @param format Container to expect `data` to be in.
 * @param data Compressed bytes.
 * @param size Number of bytes in data.
 * @param out Buffer to receive the decompressed output.
 * @param out_capacity Size of out.
 * @param out_size Receives the number of bytes written to out.
 */
bruce_result_t decompress__compute(
    bruce_compress_format_t format, const void *data, size_t size, void *out, size_t out_capacity, size_t *out_size
);

/**
 * @brief Starts a new streaming compression.
 *
 * @param format Output container. Must not be BRUCE_COMPRESS_FORMAT_AUTO.
 * @param level BRUCE_COMPRESS_LEVEL_DEFAULT, or 0-9.
 * @return A new context, or NULL on allocation failure or an invalid format/level.
 */
bruce_compress_ctx_t *compress__start(bruce_compress_format_t format, int level);

/**
 * @brief Feeds input to and/or drains output from an in-progress compression.
 *
 * Usage loop: call with a new chunk of `in`; if `*out_written` comes back
 * equal to `out_capacity`, more compressed output may be pending - call
 * again with `in`/`in_size` as NULL/0 (same `finish`) to keep draining
 * before feeding more input. Pass `finish=true` on the call carrying the
 * last input byte, then keep draining (in=NULL) until `*out_finished` comes
 * back true - that is the last chunk of compressed output.
 *
 * @param ctx Context returned by compress__start().
 * @param in New input bytes, or NULL to only drain pending output.
 * @param in_size Number of bytes in in, or 0.
 * @param finish True on (and after) the call carrying the last input byte.
 * @param out Buffer to receive compressed output.
 * @param out_capacity Size of out.
 * @param out_written Receives the number of bytes written to out this call.
 * @param out_finished Receives true once this was the final chunk of output (only possible when finish=true).
 */
bruce_result_t compress__update(
    bruce_compress_ctx_t *ctx, const void *in, size_t in_size, bool finish, void *out, size_t out_capacity,
    size_t *out_written, bool *out_finished
);

/**
 * @brief Frees a streaming compression context.
 *
 * Must be called exactly once per compress__start(), whether or not
 * out_finished was ever reached (an early abort is fine).
 */
void compress__end(bruce_compress_ctx_t *ctx);

/**
 * @brief Starts a new streaming decompression.
 *
 * @param format Container `in` is expected to be in. BRUCE_COMPRESS_FORMAT_AUTO
 *   sniffs zlib-vs-gzip from the stream's own header.
 * @return A new context, or NULL on allocation failure.
 */
bruce_compress_ctx_t *decompress__start(bruce_compress_format_t format);

/**
 * @brief Feeds input to and/or drains output from an in-progress decompression.
 *
 * Same drain loop as compress__update() (call again with in=NULL whenever
 * `*out_written == out_capacity`), except there's no `finish` flag to pass -
 * the compressed stream's own trailer marks its end, so `*out_finished`
 * becomes true on its own once the last byte of decompressed output has
 * been produced from the input fed so far.
 *
 * @param ctx Context returned by decompress__start().
 * @param in New compressed input bytes, or NULL to only drain pending output.
 * @param in_size Number of bytes in in, or 0.
 * @param out Buffer to receive decompressed output.
 * @param out_capacity Size of out.
 * @param out_written Receives the number of bytes written to out this call.
 * @param out_finished Receives true once the compressed stream's end has been reached.
 */
bruce_result_t decompress__update(
    bruce_compress_ctx_t *ctx, const void *in, size_t in_size, void *out, size_t out_capacity, size_t *out_written,
    bool *out_finished
);

/**
 * @brief Frees a streaming decompression context.
 *
 * Must be called exactly once per decompress__start(), whether or not
 * out_finished was ever reached (an early abort, or a corrupt/truncated
 * stream, is fine).
 */
void decompress__end(bruce_compress_ctx_t *ctx);
