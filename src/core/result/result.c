#include "core_sdk/result.h"

const char *result__to_string(int result) {
    switch (result) {
        case BRUCE_OK: return "OK";
        case BRUCE_ERR_INVALID_ARGUMENT: return "Invalid argument";
        case BRUCE_ERR_NOT_FOUND: return "Not found";
        case BRUCE_ERR_PERMISSION: return "Permission denied";
        case BRUCE_ERR_BUSY: return "Busy";
        case BRUCE_ERR_NO_MEMORY: return "Out of memory";
        case BRUCE_ERR_IO: return "I/O error";
        case BRUCE_ERR_UNSUPPORTED: return "Unsupported";
        case BRUCE_ERR_INVALID_STATE: return "Invalid state";
        case BRUCE_ERR_CANCELLED: return "Cancelled";
        case BRUCE_ERR_TIMEOUT: return "Timed out";
        case BRUCE_ERR_ALREADY_EXISTS: return "Already exists";
        case BRUCE_ERR_INVALID_PATH: return "Invalid path";
        case BRUCE_ERR_MANIFEST_INVALID: return "Invalid manifest";
        case BRUCE_ERR_ABI_MISMATCH: return "ABI mismatch";
        case BRUCE_ERR_TARGET_MISMATCH: return "Target mismatch";
        case BRUCE_ERR_RESOURCE_LIMIT: return "Resource limit reached";
        case BRUCE_ERR_NOT_FOREGROUND: return "Not in foreground";
        case BRUCE_ERR_NOT_INITIALIZED: return "Not initialized";
        case BRUCE_ERR_INTERNAL: return "Internal error";
        default: return "Unknown error";
    }
}
