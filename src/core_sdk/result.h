#pragma once

/*
 * Stable result vocabulary for the public Bruce SDK.
 *
 * A fallible SDK function returns BRUCE_OK on success and one of the negative
 * BRUCE_ERR_* values below on failure, unless its individual documentation
 * explicitly says it returns a positive process ID on success.  Result values
 * are part of the Core ABI and must not be renumbered.
 */
typedef enum {
    BRUCE_OK = 0,
    BRUCE_ERR_INVALID_ARGUMENT = -1,
    BRUCE_ERR_NOT_FOUND = -2,
    BRUCE_ERR_PERMISSION = -3,
    BRUCE_ERR_BUSY = -4,
    BRUCE_ERR_NO_MEMORY = -5,
    BRUCE_ERR_IO = -6,
    BRUCE_ERR_UNSUPPORTED = -7,
    BRUCE_ERR_INVALID_STATE = -8,
    BRUCE_ERR_CANCELLED = -9,
    BRUCE_ERR_TIMEOUT = -10,
    BRUCE_ERR_ALREADY_EXISTS = -11,
    BRUCE_ERR_INVALID_PATH = -12,
    BRUCE_ERR_MANIFEST_INVALID = -13,
    BRUCE_ERR_ABI_MISMATCH = -14,
    BRUCE_ERR_TARGET_MISMATCH = -15,
    BRUCE_ERR_RESOURCE_LIMIT = -16,
    BRUCE_ERR_NOT_FOREGROUND = -17,
    BRUCE_ERR_NOT_INITIALIZED = -18,
    BRUCE_ERR_INTERNAL = -19,
} bruce_result_t;

static inline int bruce__result_is_ok(bruce_result_t result) { return result == BRUCE_OK; }
