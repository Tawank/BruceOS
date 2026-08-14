#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

typedef struct {
    bruce_file_id_t id;
    int eof;
} bruce_file_t;

static bruce_file_t *file_cast(FILE *stream) { return (bruce_file_t *)stream; }

FILE *fopen(const char *path, const char *mode) {
    if (path == NULL || mode == NULL || path[0] != '/') return NULL;

    uint32_t flags = 0;
    if (strchr(mode, 'r') != NULL) flags |= BRUCE_STORAGE_OPEN_READ;
    if (strchr(mode, 'w') != NULL) {
        flags |= BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE;
    }
    if (strchr(mode, 'a') != NULL) {
        flags |= BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_APPEND;
    }
    if (strchr(mode, '+') != NULL) flags |= BRUCE_STORAGE_OPEN_READ | BRUCE_STORAGE_OPEN_WRITE;

    bruce_file_t *file = memory__calloc(1, sizeof(*file));
    if (file == NULL || storage__open(path, flags, &file->id) != BRUCE_OK) {
        memory__free(file);
        return NULL;
    }
    return (FILE *)file;
}

int fclose(FILE *stream) {
    if (stream == NULL) return EOF;
    bruce_file_t *file = file_cast(stream);
    int result = storage__close(file->id) == BRUCE_OK ? 0 : EOF;
    memory__free(file);
    return result;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (stream == NULL || ptr == NULL || size == 0 || count == 0) return 0;
    bruce_file_t *file = file_cast(stream);
    size_t bytes = 0;
    if (storage__read(file->id, ptr, size * count, &bytes) != BRUCE_OK) return 0;
    file->eof = bytes < size * count;
    return bytes / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    if (size == 0 || count == 0) return 0;
    size_t bytes = 0;
    if (storage__write(file_cast(stream)->id, ptr, size * count, &bytes) != BRUCE_OK) return 0;
    return bytes / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (stream == NULL) return -1;
    uint64_t position;
    int result = storage__seek(file_cast(stream)->id, offset, whence, &position) == BRUCE_OK ? 0 : -1;
    if (result == 0) file_cast(stream)->eof = 0;
    return result;
}

long ftell(FILE *stream) {
    uint64_t position = 0;
    if (stream == NULL || storage__seek(file_cast(stream)->id, 0, SEEK_CUR, &position) != BRUCE_OK) return -1;
    return (long)position;
}

int fgetc(FILE *stream) {
    unsigned char byte;
    return fread(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int fputc(int value, FILE *stream) {
    unsigned char byte = (unsigned char)value;
    return fwrite(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

char *fgets(char *buffer, int size, FILE *stream) {
    if (buffer == NULL || size <= 1 || stream == NULL) return NULL;
    int used = 0;
    while (used < size - 1) {
        int value = fgetc(stream);
        if (value == EOF) break;
        buffer[used++] = (char)value;
        if (value == '\n') break;
    }
    buffer[used] = '\0';
    return used > 0 ? buffer : NULL;
}

int feof(FILE *stream) { return stream == NULL ? 1 : file_cast(stream)->eof; }

int fprintf(FILE *stream, const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0) return length;
    size_t write_length = (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1;
    return fwrite(buffer, 1, write_length, stream) == write_length ? length : -1;
}

int rand(void) {
    static uint32_t state = 0x6d2b79f5u;
    state = state * 1664525u + 1013904223u;
    return (int)(state & 0x7fffffffu);
}
