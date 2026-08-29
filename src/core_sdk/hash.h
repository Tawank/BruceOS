#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Cryptographic and checksum digests.
 */

#define BRUCE_HASH_MD5_SIZE 16
#define BRUCE_HASH_SHA256_SIZE 32

typedef enum {
    BRUCE_HASH_MD5 = 0,
    BRUCE_HASH_SHA256,
} bruce_hash_algorithm_t;

typedef struct bruce_hash_ctx bruce_hash_ctx_t;

/**
 * @brief Starts a new streaming digest.
 *
 * Feed data to it with hash__update() as it becomes available (e.g. one
 * file-read chunk at a time), then collect the result with hash__finish().
 * Use hash__compute() instead when the whole input is already in memory.
 *
 * @param algorithm Which digest to compute.
 * @return A new context, or NULL on allocation failure.
 */
bruce_hash_ctx_t *hash__start(bruce_hash_algorithm_t algorithm);

/**
 * @brief Feeds `size` bytes of `data` into an in-progress digest.
 *
 * May be called any number of times between hash__start() and
 * hash__finish(). `ctx` is left usable after a failure; the caller still
 * owns it and must eventually pass it to hash__finish() to free it.
 *
 * @param ctx Context returned by hash__start().
 * @param data Bytes to add to the digest.
 * @param size Number of bytes in data.
 */
bruce_result_t hash__update(bruce_hash_ctx_t *ctx, const void *data, size_t size);

/**
 * @brief Finishes a digest, writing the result and freeing `ctx`.
 *
 * `ctx` is freed whether this succeeds or fails; it must not be used again
 * afterwards either way.
 *
 * @param ctx Context returned by hash__start().
 * @param out Receives the digest: BRUCE_HASH_MD5_SIZE or
 *   BRUCE_HASH_SHA256_SIZE raw bytes, matching the algorithm hash__start()
 *   was called with.
 */
bruce_result_t hash__finish(bruce_hash_ctx_t *ctx, uint8_t *out);

/**
 * @brief One-shot digest of a buffer already in memory.
 *
 * Equivalent to hash__start() + hash__update() + hash__finish() in a
 * single call.
 *
 * @param algorithm Which digest to compute.
 * @param data Buffer to digest.
 * @param size Number of bytes in data.
 * @param out Receives the digest, sized as in hash__finish().
 */
bruce_result_t hash__compute(bruce_hash_algorithm_t algorithm, const void *data, size_t size, uint8_t *out);

/**
 * @brief Incremental CRC-32.
 *
 * The poly 0xEDB88320/init/final-XOR 0xFFFFFFFF variant used by
 * zlib/gzip/PNG/PKZIP (and e.g. Python's zlib.crc32()). Pass 0 as `crc` for
 * the first chunk of a stream, then thread each call's return value into
 * the next one as `crc`; the value returned after the last chunk already
 * is the complete CRC-32 -- there's no separate "finish" step.
 *
 * @param crc 0 to start a new checksum, or a previous return value to continue one.
 * @param data Bytes to add to the checksum.
 * @param size Number of bytes in data.
 */
uint32_t hash__crc32(uint32_t crc, const void *data, size_t size);
