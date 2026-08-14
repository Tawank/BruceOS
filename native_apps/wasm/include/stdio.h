#pragma once

#include <stdarg.h>
#include <stddef.h>

typedef struct bruce_wasm_file FILE;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int snprintf(char *buffer, size_t capacity, const char *format, ...);
int sprintf(char *buffer, const char *format, ...);
int printf(const char *format, ...);
int vsnprintf(char *buffer, size_t capacity, const char *format, va_list args);
int sscanf(const char *input, const char *format, ...);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fgetc(FILE *stream);
int fputc(int value, FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int feof(FILE *stream);
int fprintf(FILE *stream, const char *format, ...);
