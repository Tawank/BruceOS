#include "core_sdk/compress.h"

#include <stdlib.h>
#include <string.h>

#include <zlib.h>

/*
 * Thin wrapper over zlib's z_stream - bruce_compress_ctx_t is just a
 * z_stream; compress__end()/decompress__end() are separate functions
 * (deflateEnd() vs inflateEnd()) since callers already know which one they
 * started with.
 *
 * The format enum maps straight onto zlib's windowBits trick
 * (deflateInit2()/inflateInit2()) rather than hand-rolling gzip's own
 * header/trailer framing - zlib already implements RFC1952 correctly.
 */

struct bruce_compress_ctx {
    z_stream stream;
};

/* `magnitude` is the actual window size selector (2^magnitude bytes,
 * 9-15) - kept separate from the format's sign/offset encoding so
 * compress__start() below can shrink just the window size, independently of
 * which wrapper format is being produced. */
static int compress__window_bits_magnitude(bruce_compress_format_t format, int magnitude) {
    switch (format) {
    case BRUCE_COMPRESS_FORMAT_RAW:
        return -magnitude;
    case BRUCE_COMPRESS_FORMAT_ZLIB:
        return magnitude;
    case BRUCE_COMPRESS_FORMAT_GZIP:
        return magnitude + 16;
    case BRUCE_COMPRESS_FORMAT_AUTO:
        return magnitude + 32;
    default:
        return magnitude;
    }
}

static int compress__window_bits(bruce_compress_format_t format) {
    return compress__window_bits_magnitude(format, 15);
}

size_t compress__bound(bruce_compress_format_t format, size_t input_size) {
    uLong bound = compressBound((uLong)input_size);
    /* compressBound()'s own margin (~13 bytes) already covers zlib-format
     * overhead (2-byte header + 4-byte Adler-32 trailer) plus deflate's own
     * worst-case block expansion. Raw deflate has no wrapper at all, so
     * that margin is already more than enough; gzip's minimum header+
     * trailer (10 + 8 bytes) is bigger than zlib's (2 + 4), so pad by the
     * 12-byte difference for gzip/auto. Both directions only ever add
     * slack, never cut into deflate's own worst-case margin. */
    if (format == BRUCE_COMPRESS_FORMAT_GZIP || format == BRUCE_COMPRESS_FORMAT_AUTO) bound += 12;
    return (size_t)bound;
}

bruce_compress_ctx_t *compress__start(bruce_compress_format_t format, int level) {
    if (format == BRUCE_COMPRESS_FORMAT_AUTO) return NULL;
    if (level != BRUCE_COMPRESS_LEVEL_DEFAULT && (level < 0 || level > 9)) return NULL;

    bruce_compress_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;

    /* deflate's own memory footprint is roughly (1 << (windowBits+2)) +
     * (1 << (memLevel+9)) bytes (see zlib.h) - about 256KB combined at the
     * library's usual defaults (windowBits=15, memLevel=8). zlib asks its
     * allocator for that as several separate multi-ten-KB blocks, and on a
     * long-running embedded heap the *total* free memory can be plentiful
     * while no single free block is big enough, which deflateInit2() surfaces
     * as Z_MEM_ERROR. Step window size and memLevel down together until it
     * actually succeeds (or we run out of legal values): the largest request
     * that fits wins, so a device with memory to spare still gets the best
     * ratio and a constrained one gets a working (if less space-efficient)
     * compressor instead of a hard failure. */
    static const int MAGNITUDES[] = {15, 12, 10, 9};
    static const int MEM_LEVELS[] = {8, 6, 4, 1};
    int rc = Z_MEM_ERROR;
    for (size_t i = 0; i < sizeof(MAGNITUDES) / sizeof(MAGNITUDES[0]); ++i) {
        memset(&ctx->stream, 0, sizeof(ctx->stream));
        int window_bits = compress__window_bits_magnitude(format, MAGNITUDES[i]);
        rc = deflateInit2(&ctx->stream, level, Z_DEFLATED, window_bits, MEM_LEVELS[i], Z_DEFAULT_STRATEGY);
        if (rc == Z_OK) break;
        if (rc != Z_MEM_ERROR) break; /* not a memory problem - smaller windows won't fix it */
    }
    if (rc != Z_OK) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

bruce_result_t compress__update(
    bruce_compress_ctx_t *ctx, const void *in, size_t in_size, bool finish, void *out, size_t out_capacity,
    size_t *out_written, bool *out_finished
) {
    if (ctx == NULL || out == NULL || out_written == NULL || out_finished == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (in_size > 0 && in == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    /* Only replace next_in/avail_in when the caller actually hands us a new
     * chunk. A drain-only call (in_size == 0, per this function's own doc
     * comment) must leave them alone: a tiny out_capacity can make deflate()
     * fill the whole output buffer - and so stop - before consuming any of
     * the input it was just given (e.g. gzip's fixed header alone can eat an
     * 8-byte out_capacity), leaving avail_in > 0. Clobbering that leftover
     * with 0 here would silently drop it instead of feeding it on the next
     * call. */
    if (in_size > 0) {
        ctx->stream.next_in = (z_const Bytef *)in;
        ctx->stream.avail_in = (uInt)in_size;
    }
    ctx->stream.next_out = (Bytef *)out;
    ctx->stream.avail_out = (uInt)out_capacity;

    int rc = deflate(&ctx->stream, finish ? Z_FINISH : Z_NO_FLUSH);
    *out_written = out_capacity - ctx->stream.avail_out;
    *out_finished = rc == Z_STREAM_END;
    /* Z_BUF_ERROR just means no progress was possible this call (e.g.
     * draining with nothing left to drain) - not a real error, see
     * deflate()'s own doc comment in zlib.h. */
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return BRUCE_ERR_IO;
    return BRUCE_OK;
}

void compress__end(bruce_compress_ctx_t *ctx) {
    if (ctx == NULL) return;
    deflateEnd(&ctx->stream);
    free(ctx);
}

bruce_compress_ctx_t *decompress__start(bruce_compress_format_t format) {
    bruce_compress_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;
    memset(&ctx->stream, 0, sizeof(ctx->stream));

    int rc = inflateInit2(&ctx->stream, compress__window_bits(format));
    if (rc != Z_OK) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

bruce_result_t decompress__update(
    bruce_compress_ctx_t *ctx, const void *in, size_t in_size, void *out, size_t out_capacity, size_t *out_written,
    bool *out_finished
) {
    if (ctx == NULL || out == NULL || out_written == NULL || out_finished == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (in_size > 0 && in == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    /* See the matching comment in compress__update() - a drain-only call
     * (in_size == 0) must not clobber avail_in when the previous call left
     * input unconsumed. */
    if (in_size > 0) {
        ctx->stream.next_in = (z_const Bytef *)in;
        ctx->stream.avail_in = (uInt)in_size;
    }
    ctx->stream.next_out = (Bytef *)out;
    ctx->stream.avail_out = (uInt)out_capacity;

    int rc = inflate(&ctx->stream, Z_NO_FLUSH);
    *out_written = out_capacity - ctx->stream.avail_out;
    *out_finished = rc == Z_STREAM_END;
    /* Z_BUF_ERROR: no progress possible this call, same as compress__update()
     * above - not fatal. Anything else (Z_DATA_ERROR/Z_MEM_ERROR/
     * Z_NEED_DICT/Z_STREAM_ERROR) means the input is corrupt/truncated or
     * ctx was misused. */
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return BRUCE_ERR_IO;
    return BRUCE_OK;
}

void decompress__end(bruce_compress_ctx_t *ctx) {
    if (ctx == NULL) return;
    inflateEnd(&ctx->stream);
    free(ctx);
}

bruce_result_t compress__compute(
    bruce_compress_format_t format, int level, const void *data, size_t size, void *out, size_t out_capacity,
    size_t *out_size
) {
    if (out == NULL || out_size == NULL || (data == NULL && size > 0)) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_compress_ctx_t *ctx = compress__start(format, level);
    if (ctx == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    size_t written = 0;
    bool finished = false;
    /* out_capacity >= compress__bound(format, size) guarantees deflate()
     * finishes in this single Z_FINISH call - see deflate()'s doc comment
     * in zlib.h ("Z_FINISH can be used in the first deflate call ... deflate
     * is guaranteed to return Z_STREAM_END" when enough output space is
     * given up front). */
    bruce_result_t result = compress__update(ctx, data, size, true, out, out_capacity, &written, &finished);
    compress__end(ctx);
    if (result != BRUCE_OK) return result;
    if (!finished) return BRUCE_ERR_RESOURCE_LIMIT;

    *out_size = written;
    return BRUCE_OK;
}

bruce_result_t decompress__compute(
    bruce_compress_format_t format, const void *data, size_t size, void *out, size_t out_capacity, size_t *out_size
) {
    if (out == NULL || out_size == NULL || (data == NULL && size > 0)) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_compress_ctx_t *ctx = decompress__start(format);
    if (ctx == NULL) return BRUCE_ERR_NO_MEMORY;

    size_t written = 0;
    bool finished = false;
    bruce_result_t result = decompress__update(ctx, data, size, out, out_capacity, &written, &finished);
    decompress__end(ctx);
    if (result != BRUCE_OK) return result;
    /* Not finished after consuming everything means either out_capacity was
     * too small, or the input was truncated - either way the caller's
     * "decompressed size is already known" assumption (see decompress__compute()'s
     * doc comment) didn't hold. */
    if (!finished) return BRUCE_ERR_RESOURCE_LIMIT;

    *out_size = written;
    return BRUCE_OK;
}
