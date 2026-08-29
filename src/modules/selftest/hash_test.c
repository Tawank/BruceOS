#include "hash_test.h"

#include "core_sdk/hash.h"

#include <stdio.h>
#include <string.h>

static void selftest__hash_format_hex(const uint8_t *digest, size_t size, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0Fu];
    }
    out[size * 2] = '\0';
}

bool selftest__run_hash_crc32_case(void) {
    /* "123456789" is the standard CRC-32 check value used to validate a
     * given table/algorithm against every other implementation of this
     * exact variant (poly 0xEDB88320, init/xorout 0xFFFFFFFF). */
    uint32_t one_shot = hash__crc32(0, "123456789", 9);
    bool one_shot_ok = one_shot == 0xCBF43926u;

    /* Threading the running value across calls (the documented streaming
     * contract) must produce the same result as one call over the whole
     * buffer. */
    uint32_t streamed = hash__crc32(0, "123", 3);
    streamed = hash__crc32(streamed, "456", 3);
    streamed = hash__crc32(streamed, "789", 3);
    bool streamed_ok = streamed == one_shot;

    bool empty_ok = hash__crc32(0, "", 0) == 0;

    bool ok = one_shot_ok && streamed_ok && empty_ok;
    printf(
        "[selftest] hash/crc32: %s (one_shot=%08x streamed=%08x)\n", ok ? "OK" : "FAIL",
        (unsigned)one_shot, (unsigned)streamed
    );
    return ok;
}

bool selftest__run_hash_md5_case(void) {
    uint8_t digest[BRUCE_HASH_MD5_SIZE];
    char hex[BRUCE_HASH_MD5_SIZE * 2 + 1];

    /* RFC 1321's own test vector for MD5("abc"). */
    bruce_result_t compute_result = hash__compute(BRUCE_HASH_MD5, "abc", 3, digest);
    selftest__hash_format_hex(digest, sizeof(digest), hex);
    bool compute_ok = compute_result == BRUCE_OK && strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0;

    /* Streaming (hash__start()/hash__update()/hash__finish()) must agree
     * with the one-shot hash__compute() above for the same input, split
     * across more than one update() call. */
    bruce_hash_ctx_t *ctx = hash__start(BRUCE_HASH_MD5);
    bruce_result_t update_result =
        ctx != NULL ? hash__update(ctx, "ab", 2) : BRUCE_ERR_NO_MEMORY;
    if (update_result == BRUCE_OK) update_result = hash__update(ctx, "c", 1);
    bruce_result_t finish_result = ctx != NULL ? hash__finish(ctx, digest) : update_result;
    selftest__hash_format_hex(digest, sizeof(digest), hex);
    bool stream_ok = update_result == BRUCE_OK && finish_result == BRUCE_OK &&
                      strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0;

    /* hash__update()/hash__finish() reject a NULL context instead of
     * crashing, and don't leak one that was never finished. */
    bruce_hash_ctx_t *unused_ctx = hash__start(BRUCE_HASH_MD5);
    bool null_arg_ok = hash__update(NULL, "x", 1) == BRUCE_ERR_INVALID_ARGUMENT &&
                        (unused_ctx == NULL || hash__finish(unused_ctx, NULL) == BRUCE_ERR_INVALID_ARGUMENT);

    bool ok = compute_ok && stream_ok && null_arg_ok;
    printf("[selftest] hash/md5: %s (%s)\n", ok ? "OK" : "FAIL", hex);
    return ok;
}

bool selftest__run_hash_sha256_case(void) {
    uint8_t digest[BRUCE_HASH_SHA256_SIZE];
    char hex[BRUCE_HASH_SHA256_SIZE * 2 + 1];

    /* FIPS 180-4's own test vector for SHA-256("abc"). */
    bruce_result_t compute_result = hash__compute(BRUCE_HASH_SHA256, "abc", 3, digest);
    selftest__hash_format_hex(digest, sizeof(digest), hex);
    bool compute_ok =
        compute_result == BRUCE_OK &&
        strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0;

    bruce_hash_ctx_t *ctx = hash__start(BRUCE_HASH_SHA256);
    bruce_result_t update_result =
        ctx != NULL ? hash__update(ctx, "ab", 2) : BRUCE_ERR_NO_MEMORY;
    if (update_result == BRUCE_OK) update_result = hash__update(ctx, "c", 1);
    bruce_result_t finish_result = ctx != NULL ? hash__finish(ctx, digest) : update_result;
    selftest__hash_format_hex(digest, sizeof(digest), hex);
    bool stream_ok =
        update_result == BRUCE_OK && finish_result == BRUCE_OK &&
        strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0;

    bool ok = compute_ok && stream_ok;
    printf("[selftest] hash/sha256: %s (%s)\n", ok ? "OK" : "FAIL", hex);
    return ok;
}
