#pragma once

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *left, const void *right, size_t size);
size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
char *strchr(const char *text, int value);
