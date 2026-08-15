#include "core/image/image.h"

#include <string.h>

bool image__reader_read(image_reader_t *reader, void *out, size_t size) {
    if (reader->offset > reader->size || size > reader->size - reader->offset) return false;
    if (out != NULL) memcpy(out, reader->data + reader->offset, size);
    reader->offset += size;
    return true;
}

bool image__reader_skip(image_reader_t *reader, size_t size) {
    return image__reader_read(reader, NULL, size);
}

bool image__reader_read_u8(image_reader_t *reader, uint8_t *out) {
    return image__reader_read(reader, out, 1);
}

bool image__reader_read_u16(image_reader_t *reader, uint16_t *out) {
    uint8_t bytes[2];
    if (!image__reader_read(reader, bytes, sizeof(bytes))) return false;
    *out = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return true;
}
