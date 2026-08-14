#pragma once

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void exit(int status);
int rand(void);
long strtol(const char *text, char **out_end, int base);
double strtod(const char *text, char **out_end);
