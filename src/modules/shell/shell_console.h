#pragma once

#include <stdbool.h>
#include <stddef.h>

/* `prompt` is written as-is ahead of the line (and on every redraw); pass
 * NULL for the shell's normal "bruce$ " prompt, or a continuation prompt
 * (see shell_console__continuation_prompt()) while reading another physical
 * line into an still-incomplete if/fi or function block (see
 * shell_compound__pending() in shell_compound.c). */
int shell_console__read_line(char *line, size_t capacity, bool *skip_lf, const char *prompt);

/* The "> " continuation prompt shown while shell__interactive() (shell_app.c)
 * is still accumulating a multi-line if/fi or function block. */
const char *shell_console__continuation_prompt(void);
void shell_console__reset_ready(void);
bool shell_console__is_ready(void);
