#pragma once

/* Non-static forward declarations for the ELF loader's FILE*-based stdio
 * adapters (see elf_loader_sdk_symbols.c, above bruce_elf__fopen()), so
 * modules/selftest can exercise the mode-parsing/flag-mapping/eof-tracking
 * logic directly without a full cross-compiled ELF fixture. Every other
 * bruce_elf__* adapter in that file stays `static` and is not declared
 * here. */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "elf_loader_internal.h"

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
void bruce_elf__perror(const char *prefix);
extern FILE *bruce_elf__stdin_ptr;
extern FILE *bruce_elf__stdout_ptr;
extern FILE *bruce_elf__stderr_ptr;

char *bruce_elf__getenv(const char *name);
int bruce_elf__setenv(const char *name, const char *value, int overwrite);
int bruce_elf__unsetenv(const char *name);
char *bruce_elf__strdup(const char *text);
char *bruce_elf__strndup(const char *text, size_t size);

struct tm *bruce_elf__localtime_r(const time_t *timer, struct tm *out);
struct tm *bruce_elf__localtime(const time_t *timer);
time_t bruce_elf__mktime(struct tm *tm);
clock_t bruce_elf__clock(void);

int bruce_elf__open(const char *path, int flags, ...);
int bruce_elf__close(int fd);
ssize_t bruce_elf__read(int fd, void *buffer, size_t count);
ssize_t bruce_elf__write(int fd, const void *buffer, size_t count);
off_t bruce_elf__lseek(int fd, off_t offset, int whence);
int bruce_elf__stat(const char *path, struct stat *out);
int bruce_elf__fstat(int fd, struct stat *out);
int bruce_elf__mkdir(const char *path, mode_t mode);
int bruce_elf__access(const char *path, int mode);
DIR *bruce_elf__opendir(const char *path);
struct dirent *bruce_elf__readdir(DIR *dirp);
void bruce_elf__rewinddir(DIR *dirp);
int bruce_elf__closedir(DIR *dirp);

_Noreturn void bruce_elf__exit(int status);
_Noreturn void bruce_elf__abort(void);
