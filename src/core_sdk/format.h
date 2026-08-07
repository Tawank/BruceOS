#pragma once

#include <stddef.h>
#include <stdint.h>

/* Formats `bytes` as a short human-readable string using 1024-based units
 * (K, M, G, T) -- e.g. 500 -> "500", 8400 -> "8.2K", 8832614 -> "8.4M". Plain
 * byte counts (under 1024) get no unit suffix, just the number. A single
 * decimal digit is shown once the value no longer fits in whole units;
 * exact multiples never get a decimal point. Always NUL-terminates within
 * `capacity`; a NULL output or zero capacity is a no-op. */
void format__bytes_human(uint64_t bytes, char *output, size_t capacity);
