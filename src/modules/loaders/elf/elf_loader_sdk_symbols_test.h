#pragma once

/* Non-static forward declarations for the ELF loader's FILE*-based stdio
 * adapters (see elf_loader_sdk_symbols.c, above bruce_elf__fopen()), so
 * modules/selftest can exercise the mode-parsing/flag-mapping/eof-tracking
 * logic directly without a full cross-compiled ELF fixture. Every other
 * bruce_elf__* adapter in that file stays `static` and is not declared
 * here. */

#include <stdarg.h>
#include <stdio.h>

FILE *bruce_elf__fopen(const char *path, const char *mode);
int bruce_elf__fclose(FILE *stream);
size_t bruce_elf__fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t bruce_elf__fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int bruce_elf__fseek(FILE *stream, long offset, int whence);
long bruce_elf__ftell(FILE *stream);
void bruce_elf__rewind(FILE *stream);
int bruce_elf__fflush(FILE *stream);
int bruce_elf__setvbuf(FILE *stream, char *buffer, int mode, size_t size);
int bruce_elf__fgetc(FILE *stream);
int bruce_elf__fputc(int character, FILE *stream);
char *bruce_elf__fgets(char *buffer, int size, FILE *stream);
int bruce_elf__fputs(const char *text, FILE *stream);
int bruce_elf__feof(FILE *stream);
int bruce_elf__ferror(FILE *stream);
void bruce_elf__clearerr(FILE *stream);
int bruce_elf__remove(const char *path);
int bruce_elf__rename(const char *from, const char *to);
int bruce_elf__fprintf(FILE *stream, const char *format, ...);
int bruce_elf__vfprintf(FILE *stream, const char *format, va_list args);
extern FILE *bruce_elf__stdin_ptr;
extern FILE *bruce_elf__stdout_ptr;
extern FILE *bruce_elf__stderr_ptr;

char *bruce_elf__getenv(const char *name);
int bruce_elf__setenv(const char *name, const char *value, int overwrite);
int bruce_elf__unsetenv(const char *name);
char *bruce_elf__strdup(const char *text);
char *bruce_elf__strndup(const char *text, size_t size);
