#include "base64_test.h"

#include "core_sdk/base64.h"

#include <stdio.h>
#include <string.h>

/* RFC 4648 section 10's own worked examples, covering all three padding
 * lengths (0, 1, 2 '=' characters) plus the empty string. */
typedef struct {
    const char *decoded;
    const char *encoded;
} selftest__base64_vector_t;

static const selftest__base64_vector_t selftest__base64_vectors[] = {
    {"", ""},
    {"f", "Zg=="},
    {"fo", "Zm8="},
    {"foo", "Zm9v"},
    {"foob", "Zm9vYg=="},
    {"fooba", "Zm9vYmE="},
    {"foobar", "Zm9vYmFy"},
};

bool selftest__run_base64_encode_case(void) {
    bool ok = true;
    for (size_t i = 0; i < sizeof(selftest__base64_vectors) / sizeof(selftest__base64_vectors[0]); ++i) {
        const selftest__base64_vector_t *vector = &selftest__base64_vectors[i];
        size_t decoded_length = strlen(vector->decoded);
        char out[16];
        bruce_result_t result = base64__encode(vector->decoded, decoded_length, out, sizeof(out));
        if (result != BRUCE_OK || strcmp(out, vector->encoded) != 0) {
            ok = false;
            printf(
                "[selftest] hash/base64_encode: mismatch for %-6s -> %s (want %s, result %d)\n",
                vector->decoded, result == BRUCE_OK ? out : "(error)", vector->encoded, result
            );
        }
    }

    /* A too-small output buffer is rejected instead of overflowing. */
    char tiny[2];
    bruce_result_t small_result = base64__encode("foobar", 6, tiny, sizeof(tiny));
    bool small_ok = small_result == BRUCE_ERR_RESOURCE_LIMIT;

    ok = ok && small_ok;
    printf("[selftest] hash/base64_encode: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_base64_decode_case(void) {
    bool ok = true;
    for (size_t i = 0; i < sizeof(selftest__base64_vectors) / sizeof(selftest__base64_vectors[0]); ++i) {
        const selftest__base64_vector_t *vector = &selftest__base64_vectors[i];
        size_t encoded_length = strlen(vector->encoded);
        uint8_t out[16] = {0};
        size_t out_size = 0;
        bruce_result_t result = base64__decode(vector->encoded, encoded_length, out, sizeof(out), &out_size);
        bool matches = result == BRUCE_OK && out_size == strlen(vector->decoded) &&
                        memcmp(out, vector->decoded, out_size) == 0;
        if (!matches) {
            ok = false;
            printf("[selftest] hash/base64_decode: mismatch decoding %s (result %d)\n", vector->encoded, result);
        }
    }

    /* Whitespace/newlines embedded in wrapped text (as base64(1) or this
     * project's own "base64" command would produce) are ignored. */
    uint8_t wrapped_out[16] = {0};
    size_t wrapped_size = 0;
    bruce_result_t wrapped_result =
        base64__decode("Zm9v\nYmFy\n", 10, wrapped_out, sizeof(wrapped_out), &wrapped_size);
    bool wrapped_ok =
        wrapped_result == BRUCE_OK && wrapped_size == 6 && memcmp(wrapped_out, "foobar", 6) == 0;

    /* Malformed input is rejected rather than silently misdecoded. */
    uint8_t scratch[16];
    size_t scratch_size = 0;
    bruce_result_t bad_char_result = base64__decode("Zm9v!", 5, scratch, sizeof(scratch), &scratch_size);
    bruce_result_t bad_padding_result = base64__decode("AA=A", 4, scratch, sizeof(scratch), &scratch_size);
    bruce_result_t bad_length_result = base64__decode("Zm9", 3, scratch, sizeof(scratch), &scratch_size);
    bool malformed_ok = bad_char_result == BRUCE_ERR_INVALID_ARGUMENT &&
                         bad_padding_result == BRUCE_ERR_INVALID_ARGUMENT &&
                         bad_length_result == BRUCE_ERR_INVALID_ARGUMENT;

    /* A too-small output buffer is rejected instead of overflowing. */
    uint8_t tiny[1];
    size_t tiny_size = 0;
    bruce_result_t small_result = base64__decode("Zm9vYmFy", 8, tiny, sizeof(tiny), &tiny_size);
    bool small_ok = small_result == BRUCE_ERR_RESOURCE_LIMIT;

    ok = ok && wrapped_ok && malformed_ok && small_ok;
    printf(
        "[selftest] hash/base64_decode: %s (wrapped=%d bad_char=%d bad_padding=%d bad_length=%d small=%d)\n",
        ok ? "OK" : "FAIL", wrapped_result, bad_char_result, bad_padding_result, bad_length_result, small_result
    );
    return ok;
}
