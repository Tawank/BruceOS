#include "core_sdk/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"

#include "core/memory/memory.h"
#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

#define LOADER__IO_CHUNK 4096u
#define LOADER__ERROR_MESSAGE_MAX 128u

static char s_error_message[LOADER__ERROR_MESSAGE_MAX];

void loader__set_error_message(const char *message) {
    if (message == NULL) {
        s_error_message[0] = '\0';
        return;
    }
    strncpy(s_error_message, message, sizeof(s_error_message) - 1);
    s_error_message[sizeof(s_error_message) - 1] = '\0';
}

const char *loader__last_error_message(void) { return s_error_message; }

void loader__format_error_message(const char *action, int result, char *out_message, size_t out_size) {
    if (out_message == NULL || out_size == 0) return;
    const char *detail = result == BRUCE_ERR_ABI_MISMATCH ? loader__last_error_message() : NULL;
    if (detail != NULL && detail[0] != '\0') {
        snprintf(
            out_message,
            out_size,
            "Can't load this app. A required function is missing. Try updating Bruce to the latest "
            "version.\n%s",
            detail
        );
        return;
    }
    snprintf(
        out_message,
        out_size,
        "%s failed: %s (%d)",
        action != NULL && action[0] != '\0' ? action : "Launch",
        app_runner__result_to_string(result),
        result
    );
}

bruce_result_t loader__stage_path(const char *path, bruce_loader_t *out_image) {
    if (path == NULL || out_image == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(out_image, 0, sizeof(*out_image));

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    uint64_t file_size = 0;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_END, &file_size);
    if (result == BRUCE_OK && (file_size == 0 || file_size >= SIZE_MAX)) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
    }
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);
    if (result != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        return result;
    }

    result = memory__external_alloc((size_t)file_size + 1u, &out_image->memory);
    uint8_t *buffer = NULL;
    if (result == BRUCE_OK) {
        buffer = malloc(LOADER__IO_CHUNK);
        if (buffer == NULL) result = BRUCE_ERR_NO_MEMORY;
    }

    uint32_t source_crc = 0;
    size_t offset = 0;
    while (result == BRUCE_OK && offset < (size_t)file_size) {
        size_t wanted =
            (size_t)file_size - offset < LOADER__IO_CHUNK ? (size_t)file_size - offset : LOADER__IO_CHUNK;
        size_t received = 0;
        result = storage__read(file, buffer, wanted, &received);
        if (result != BRUCE_OK || received != wanted) {
            if (result == BRUCE_OK) result = BRUCE_ERR_IO;
            break;
        }
        source_crc = esp_rom_crc32_le(source_crc, buffer, received);
        result = memory__external_write(&out_image->memory, offset, buffer, received);
        offset += received;
    }
    static const uint8_t terminator = 0;
    if (result == BRUCE_OK) {
        result = memory__external_write(&out_image->memory, (size_t)file_size, &terminator, 1);
    }
    free(buffer);
    bruce_result_t close_result = storage__close(file);
    if (result == BRUCE_OK) result = close_result;
    if (result == BRUCE_OK) {
        const void *data = NULL;
        result = memory__external_map(&out_image->memory, &data);
#if !CONFIG_BRUCE_QEMU_TEST_MODE
        if (result == BRUCE_OK && esp_rom_crc32_le(0, data, (size_t)file_size) != source_crc) {
            result = BRUCE_ERR_IO;
        }
#endif
        if (result == BRUCE_OK) {
            out_image->data = data;
            out_image->size = (size_t)file_size;
        }
    }
    if (result != BRUCE_OK) {
        if (out_image->memory.handle != 0) (void)memory__external_free(&out_image->memory);
        memset(out_image, 0, sizeof(*out_image));
    }
    return result;
}

bruce_result_t loader__release_image(bruce_loader_t *image) {
    if (image == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = memory__external_free(&image->memory);
    if (result == BRUCE_OK) memset(image, 0, sizeof(*image));
    return result;
}

bruce_result_t loader__adopt_image(bruce_loader_t *image) {
    if (image == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return memory_external__adopt(&image->memory);
}

bruce_result_t loader__allocate_xip(size_t size, bruce_loader_xip_image_t *out_image) {
    if (out_image == NULL || size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(out_image, 0, sizeof(*out_image));
    bruce_result_t result = memory_external__alloc(size, true, &out_image->memory);
    const void *instruction = NULL;
    const void *data = NULL;
    if (result == BRUCE_OK) { result = memory_external__instruction_map(&out_image->memory, &instruction); }
    if (result == BRUCE_OK) result = memory__external_map(&out_image->memory, &data);
    if (result != BRUCE_OK) {
        if (out_image->memory.handle != 0) { (void)memory_external__release(&out_image->memory); }
        return result;
    }
    out_image->instruction = instruction;
    out_image->data = data;
    out_image->size = out_image->memory.size;
    return BRUCE_OK;
}

bruce_result_t
loader__write_xip(const bruce_loader_xip_image_t *image, size_t offset, const void *data, size_t size) {
    if (image == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return memory__external_write(&image->memory, offset, data, size);
}

bruce_result_t loader__adopt_xip(bruce_loader_xip_image_t *image) {
    if (image == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return memory_external__adopt(&image->memory);
}

bruce_result_t loader__release_xip(bruce_loader_xip_image_t *image) {
    if (image == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = memory_external__release(&image->memory);
    if (result == BRUCE_OK) memset(image, 0, sizeof(*image));
    return result;
}
