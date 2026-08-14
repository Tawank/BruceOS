/* In-memory libc functions needed by Bruce WASM guests. No host libc or WASI calls. */

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

int errno;

int printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = stdio__vprintf(format, args);
    va_end(args);
    return result;
}

void exit(int status) { (void)status; }

int sprintf(char *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, SIZE_MAX, format, args);
    va_end(args);
    return result;
}

void *malloc(size_t size) { return memory__malloc(size); }
void *calloc(size_t count, size_t size) { return memory__calloc(count, size); }
void *realloc(void *pointer, size_t size) { return memory__realloc(pointer, size); }
void free(void *pointer) { memory__free(pointer); }

void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
    return destination;
}

void *memset(void *destination, int value, size_t size) {
    unsigned char *out = destination;
    for (size_t i = 0; i < size; ++i) out[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t size) {
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

char *strcpy(char *destination, const char *source) {
    char *result = destination;
    while ((*destination++ = *source++) != '\0') {}
    return result;
}

char *strncpy(char *destination, const char *source, size_t size) {
    char *result = destination;
    while (size > 0 && (*destination++ = *source++) != '\0') size--;
    while (size-- > 0) *destination++ = '\0';
    return result;
}

char *strcat(char *destination, const char *source) {
    strcpy(destination + strlen(destination), source);
    return destination;
}

size_t strlen(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') length++;
    return length;
}

int strcmp(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        left++;
        right++;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t size) {
    while (size-- > 0 && *left != '\0' && *left == *right) {
        left++;
        right++;
    }
    return size == SIZE_MAX ? 0 : (unsigned char)*left - (unsigned char)*right;
}

char *strchr(const char *text, int value) {
    char match = (char)value;
    do {
        if (*text == match) return (char *)text;
    } while (*text++ != '\0');
    return NULL;
}

char *strrchr(const char *text, int value) {
    const char *result = NULL;
    do {
        if (*text == (char)value) result = text;
    } while (*text++ != '\0');
    return (char *)result;
}

char *strncat(char *destination, const char *source, size_t size) {
    char *result = destination;
    while (*destination != '\0') destination++;
    while (size-- > 0 && (*destination++ = *source++) != '\0') {}
    if (destination[-1] != '\0') *destination = '\0';
    return result;
}

int isdigit(int value) { return value >= '0' && value <= '9'; }

int isspace(int value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

long strtol(const char *text, char **out_end, int base) {
    const char *cursor = text;
    while (isspace((unsigned char)*cursor)) cursor++;
    bool negative = *cursor == '-';
    if (*cursor == '-' || *cursor == '+') cursor++;
    if (base == 0) {
        if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
            base = 16;
            cursor += 2;
        } else {
            base = cursor[0] == '0' ? 8 : 10;
        }
    }
    const char *digits = cursor;
    unsigned long value = 0;
    while (*cursor != '\0') {
        unsigned int digit;
        if (*cursor >= '0' && *cursor <= '9') digit = (unsigned int)(*cursor - '0');
        else if (*cursor >= 'a' && *cursor <= 'z') digit = (unsigned int)(*cursor - 'a') + 10;
        else if (*cursor >= 'A' && *cursor <= 'Z') digit = (unsigned int)(*cursor - 'A') + 10;
        else break;
        if (digit >= (unsigned int)base) break;
        if (value > (UINT32_MAX - digit) / (unsigned int)base) {
            errno = ERANGE;
            value = UINT32_MAX;
        } else if (errno != ERANGE) {
            value = value * (unsigned int)base + digit;
        }
        cursor++;
    }
    if (cursor == digits) cursor = text;
    if (out_end != NULL) *out_end = (char *)cursor;
    return negative ? -(long)value : (long)value;
}

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} wasm_format_output_t;

/* wasi-libc's allocator uses a 128-bit overflow check. Clang normally supplies
 * this compiler-rt routine; keep it guest-local so it cannot become an import. */
__int128 __multi3(__int128 left, __int128 right) {
    unsigned __int128 a = (unsigned __int128)left;
    unsigned __int128 b = (unsigned __int128)right;
    unsigned __int128 result = 0;
    while (b != 0) {
        if ((b & 1) != 0) result += a;
        a <<= 1;
        b >>= 1;
    }
    return (__int128)result;
}

static void wasm_format__character(wasm_format_output_t *output, char value) {
    if (output->capacity > 0 && output->length + 1 < output->capacity) output->buffer[output->length] = value;
    output->length++;
}

static void wasm_format__padding(wasm_format_output_t *output, char value, int count) {
    while (count-- > 0) wasm_format__character(output, value);
}

static int wasm_format__unsigned(
    wasm_format_output_t *output, unsigned long value, unsigned int base, bool upper, int width, char padding
) {
    char digits[32];
    int count = 0;
    do {
        unsigned int digit = (unsigned int)(value % base);
        digits[count++] = (char)(digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
        value /= base;
    } while (value != 0);
    wasm_format__padding(output, padding, width - count);
    while (count > 0) wasm_format__character(output, digits[--count]);
    return 0;
}

int vsnprintf(char *buffer, size_t capacity, const char *format, va_list args) {
    if (format == NULL || (buffer == NULL && capacity != 0)) return -1;
    wasm_format_output_t output = {.buffer = buffer, .capacity = capacity, .length = 0};
    while (*format != '\0') {
        if (*format != '%') {
            wasm_format__character(&output, *format++);
            continue;
        }
        format++;
        if (*format == '%') {
            wasm_format__character(&output, *format++);
            continue;
        }
        char padding = ' ';
        if (*format == '0') {
            padding = '0';
            format++;
        }
        int width = 0;
        if (*format == '*') {
            width = va_arg(args, int);
            format++;
        } else {
            while (isdigit((unsigned char)*format)) width = width * 10 + (*format++ - '0');
        }
        int precision = -1;
        if (*format == '.') {
            format++;
            if (*format == '*') {
                precision = va_arg(args, int);
                format++;
            } else {
                precision = 0;
                while (isdigit((unsigned char)*format)) precision = precision * 10 + (*format++ - '0');
            }
        }
        bool long_value = false;
        if (*format == 'l') {
            long_value = true;
            format++;
        }
        char conversion = *format++;
        if (conversion == 's') {
            const char *text = va_arg(args, const char *);
            if (text == NULL) text = "(null)";
            int length = 0;
            while (text[length] != '\0' && (precision < 0 || length < precision)) length++;
            wasm_format__padding(&output, ' ', width - length);
            for (int i = 0; i < length; ++i) wasm_format__character(&output, text[i]);
        } else if (conversion == 'c') {
            wasm_format__padding(&output, ' ', width - 1);
            wasm_format__character(&output, (char)va_arg(args, int));
        } else if (conversion == 'd' || conversion == 'i') {
            long value = long_value ? va_arg(args, long) : va_arg(args, int);
            unsigned long magnitude = value < 0 ? (unsigned long)(-(value + 1)) + 1 : (unsigned long)value;
            if (value < 0) {
                wasm_format__character(&output, '-');
                if (width > 0) width--;
            }
            wasm_format__unsigned(&output, magnitude, 10, false, width, padding);
        } else if (conversion == 'u' || conversion == 'x' || conversion == 'X') {
            unsigned long value = long_value ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
            wasm_format__unsigned(
                &output, value, conversion == 'u' ? 10 : 16, conversion == 'X', width, padding
            );
        } else {
            return -1;
        }
    }
    if (capacity > 0) output.buffer[output.length < capacity ? output.length : capacity - 1] = '\0';
    return output.length <= INT32_MAX ? (int)output.length : -1;
}

int snprintf(char *buffer, size_t capacity, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, capacity, format, args);
    va_end(args);
    return result;
}

int sscanf(const char *input, const char *format, ...) {
    if (input == NULL || format == NULL) return -1;
    va_list args;
    va_start(args, format);
    int assigned = 0;
    while (*format != '\0') {
        if (*format != '%') {
            if (*input++ != *format++) break;
            continue;
        }
        format++;
        if (*format == 'u') {
            while (isspace((unsigned char)*input)) input++;
            bool negative = *input == '-';
            if (*input == '-' || *input == '+') input++;
            if (!isdigit((unsigned char)*input)) break;
            unsigned int value = 0;
            do {
                unsigned int digit = (unsigned int)(*input++ - '0');
                if (value > (UINT32_MAX - digit) / 10) goto done;
                value = value * 10 + digit;
            } while (isdigit((unsigned char)*input));
            *va_arg(args, unsigned int *) = negative ? 0u - value : value;
            assigned++;
            format++;
        } else if (*format == 'c') {
            if (*input == '\0') break;
            *va_arg(args, char *) = *input++;
            assigned++;
            format++;
        } else {
            break;
        }
    }
done:
    va_end(args);
    return assigned;
}

double strtod(const char *text, char **out_end) {
    errno = 0;
    const char *cursor = text;
    while (isspace((unsigned char)*cursor)) cursor++;
    bool negative = *cursor == '-';
    if (*cursor == '-' || *cursor == '+') cursor++;
    bool any_digit = false;
    double value = 0.0;
    while (isdigit((unsigned char)*cursor)) {
        any_digit = true;
        value = value * 10.0 + (*cursor++ - '0');
    }
    if (*cursor == '.') {
        cursor++;
        double scale = 0.1;
        while (isdigit((unsigned char)*cursor)) {
            any_digit = true;
            value += (*cursor++ - '0') * scale;
            scale *= 0.1;
        }
    }
    if (any_digit && (*cursor == 'e' || *cursor == 'E')) {
        const char *exponent_start = cursor++;
        bool exponent_negative = *cursor == '-';
        if (*cursor == '-' || *cursor == '+') cursor++;
        if (!isdigit((unsigned char)*cursor)) {
            cursor = exponent_start;
        } else {
            unsigned int exponent = 0;
            while (isdigit((unsigned char)*cursor)) {
                if (exponent < 1000) exponent = exponent * 10 + (unsigned int)(*cursor - '0');
                cursor++;
            }
            if (exponent > 400) {
                errno = ERANGE;
                value = exponent_negative ? 0.0 : 1.0 / 0.0;
            } else {
                while (exponent-- > 0) {
                    if (!exponent_negative && value > DBL_MAX / 10.0) {
                        errno = ERANGE;
                        value = 1.0 / 0.0;
                        break;
                    }
                    double previous = value;
                    value = exponent_negative ? value / 10.0 : value * 10.0;
                    if (exponent_negative && previous != 0.0 && value == 0.0) {
                        errno = ERANGE;
                        break;
                    }
                }
            }
        }
    }
    if (!any_digit) cursor = text;
    if (out_end != NULL) *out_end = (char *)cursor;
    return negative ? -value : value;
}
