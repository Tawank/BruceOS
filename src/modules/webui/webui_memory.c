#include "webui_internal.h"

/* Kept out of ~1KiB reserved so a save never claims literally every byte a
 * stats snapshot reported free -- by the time the allocation call actually
 * runs, concurrent activity (the other http worker, a background app) may
 * have already taken some of it. */
#define WEBUI_MEMORY_RESERVE_BYTES (1024u)
#define WEBUI_DECODE_CHUNK 256u

size_t webui__memory_cap(void) {
    bruce_memory_stats_t stats;
    if (memory__get_stats(&stats) != BRUCE_OK) return 0;
    size_t cap = stats.internal_largest_block;
    if (stats.psram_largest_block > cap) cap = stats.psram_largest_block;
    if (stats.swap_largest_block > cap) cap = stats.swap_largest_block;
    return cap > WEBUI_MEMORY_RESERVE_BYTES ? cap - WEBUI_MEMORY_RESERVE_BYTES : 0;
}

bruce_result_t
webui__alloc_direct(void **out_data, bruce_memory_object_t *out_object, bool *out_external, size_t size) {
    bruce_memory_stats_t stats;
    bool psram_has_room = memory__get_stats(&stats) == BRUCE_OK && stats.psram_largest_block >= size;
    if (psram_has_room) {
        bruce_memory_object_t object;
        if (memory__external_alloc(size, &object) == BRUCE_OK) {
            const void *mapped = NULL;
            if ((object.backend == BRUCE_MEMORY_BACKEND_PSRAM ||
                 object.backend == BRUCE_MEMORY_BACKEND_INTERNAL) &&
                memory__external_map(&object, &mapped) == BRUCE_OK) {
                *out_data = (void *)mapped;
                *out_object = object;
                *out_external = true;
                return BRUCE_OK;
            }
            (void)memory__external_free(&object);
        }
    }
    *out_data = memory__malloc(size);
    *out_external = false;
    return *out_data != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
}

void webui__free_direct(void *data, bruce_memory_object_t *object, bool external) {
    if (external) (void)memory__external_free(object);
    else memory__free(data);
}

/* Worker tasks run with a 4 KiB stack (HTTP_SERVER__WORKER_STACK_SIZE), so
 * the chunk buffer below is heap-allocated rather than a local array -- this
 * function nests inside a route handler's own frame(s), and a couple of
 * kilobytes of locals stacked on top of those would risk overflowing it. */
bruce_result_t webui__receive_into_object(
    bruce_http_server_request_t *request, const bruce_memory_object_t *object, size_t length
) {
    uint8_t *chunk = memory__malloc(WEBUI_IO_CHUNK);
    if (chunk == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = BRUCE_OK;
    size_t received = 0;
    while (received < length) {
        size_t wanted = length - received;
        if (wanted > WEBUI_IO_CHUNK) wanted = WEBUI_IO_CHUNK;
        int count = http_server_request__recv(request, chunk, wanted);
        if (count <= 0) {
            result = count < 0 ? (bruce_result_t)count : BRUCE_ERR_IO;
            break;
        }
        result = memory__external_write(object, received, chunk, (size_t)count);
        if (result != BRUCE_OK) break;
        received += (size_t)count;
    }
    memory__free(chunk);
    return result;
}

bruce_result_t webui__decode_to_object(
    const char *source, size_t length, const bruce_memory_object_t *object, size_t *out_length
) {
    uint8_t *chunk = memory__malloc(WEBUI_DECODE_CHUNK);
    if (chunk == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t filled = 0;
    size_t written = 0;
    bruce_result_t result = BRUCE_OK;
    for (size_t i = 0; result == BRUCE_OK && i < length; i++) {
        unsigned char value = (unsigned char)source[i];
        if (value == '%') {
            if (i + 2u >= length) {
                result = BRUCE_ERR_INVALID_ARGUMENT;
                break;
            }
            int high = webui__hex(source[i + 1u]);
            int low = webui__hex(source[i + 2u]);
            if (high < 0 || low < 0) {
                result = BRUCE_ERR_INVALID_ARGUMENT;
                break;
            }
            value = (unsigned char)((high << 4) | low);
            i += 2u;
        } else if (value == '+') {
            value = ' ';
        }
        if (value == 0 || (value < 0x20u && value != '\t' && value != '\n' && value != '\r') ||
            value == 0x7fu) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
            break;
        }
        chunk[filled++] = value;
        if (filled == WEBUI_DECODE_CHUNK) {
            result = memory__external_write(object, written, chunk, filled);
            written += filled;
            filled = 0;
        }
    }
    if (result == BRUCE_OK && filled > 0) {
        result = memory__external_write(object, written, chunk, filled);
        written += filled;
    }
    memory__free(chunk);
    if (result == BRUCE_OK) *out_length = written;
    return result;
}
