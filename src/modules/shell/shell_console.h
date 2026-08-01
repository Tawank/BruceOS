#pragma once

#include <stdbool.h>
#include <stddef.h>

int shell_console__read_line(char *line, size_t capacity, bool *skip_lf);
void shell_console__reset_ready(void);
bool shell_console__is_ready(void);
