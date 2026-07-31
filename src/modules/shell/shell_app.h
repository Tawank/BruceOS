#pragma once

#include <stdbool.h>
#include <stddef.h>

int shell_app_main(int argc, char **argv);
int shell_loader__run_path(const char *path, const char *arg, bool in_background);

/* Wraps `text` in double quotes, escaping backslashes and embedded double
 * quotes, so the result round-trips through app_runner__parse_args() as a
 * single argv token. Returns false if `out` (size `capacity`) is too small. */
bool shell__quote_arg(const char *text, char *out, size_t capacity);
