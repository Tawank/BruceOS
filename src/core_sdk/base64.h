#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Base64 encoding (RFC 4648).
 */

/** Encoded length of `byte_count` raw bytes, not counting a NUL terminator. */
#define BRUCE_BASE64_ENCODED_SIZE(byte_count) ((((byte_count) + 2) / 3) * 4)

/** Upper bound on the decoded length of `text_length` base64 characters (before whitespace is discarded). */
#define BRUCE_BASE64_DECODED_SIZE(text_length) ((((text_length) + 3) / 4) * 3)

/**
 * @brief Encodes `size` bytes of `data` as base64 text.
 *
 * `out` receives BRUCE_BASE64_ENCODED_SIZE(size) characters plus a NUL
 * terminator; `out_capacity` must be at least that large or this fails
 * with BRUCE_ERR_RESOURCE_LIMIT without writing anything. The output is a
 * single unbroken run of characters -- a caller that wants line-wrapped
 * output (e.g. matching the classic 76-column base64(1) format) inserts
 * its own breaks when writing it out.
 *
 * @param data Bytes to encode.
 * @param size Number of bytes in data.
 * @param out Buffer to receive the encoded text and its NUL terminator.
 * @param out_capacity Size of out in bytes.
 */
bruce_result_t base64__encode(const void *data, size_t size, char *out, size_t out_capacity);

/**
 * @brief Decodes base64 text back into raw bytes.
 *
 * Whitespace (space, tab, CR, LF) anywhere in `text` is ignored, so
 * previously line-wrapped text decodes without preprocessing. Padding
 * ('=') is only accepted at the very end of the (whitespace-stripped)
 * input. Fails with BRUCE_ERR_INVALID_ARGUMENT for malformed input (bad
 * alphabet, misplaced padding, or a length that isn't a multiple of 4 once
 * whitespace is stripped), or BRUCE_ERR_RESOURCE_LIMIT if out isn't large
 * enough for BRUCE_BASE64_DECODED_SIZE(text_length) bytes. `text`/`out` may
 * be NULL when `text_length`/`out_capacity` (respectively) is 0.
 *
 * @param text Base64 text to decode.
 * @param text_length Number of characters in text.
 * @param out Buffer to receive the decoded bytes.
 * @param out_capacity Size of out in bytes.
 * @param out_size Receives the number of bytes written to out.
 */
bruce_result_t
base64__decode(const char *text, size_t text_length, uint8_t *out, size_t out_capacity, size_t *out_size);
