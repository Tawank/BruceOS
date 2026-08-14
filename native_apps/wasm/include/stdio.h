#pragma once

#include <stdarg.h>
#include <stddef.h>

int snprintf(char *buffer, size_t capacity, const char *format, ...);
int vsnprintf(char *buffer, size_t capacity, const char *format, va_list args);
int sscanf(const char *input, const char *format, ...);
