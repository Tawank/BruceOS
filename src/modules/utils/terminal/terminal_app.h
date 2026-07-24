#pragma once

/*
 * Built-in serial terminal.  Registered as the "terminal" command, it reads
 * lines from stdin and dispatches them through AppRunner: the first whitespace-
 * delimited token is the application name or file path, and the remainder is
 * forwarded as the app's argument string (parsed by app_runner__run() or
 * app_runner__run_path() with the same quoting rules as every other launch
 * path).
 *
 * The interactive entry point is terminal_app().  The parser is also exposed as
 * terminal__run_line() so that the JavaScript serial.cmd() binding can route a
 * command string to the same parser without duplicating the split logic.
 */

int terminal_app_main(int argc, char **argv);

/* Parses a single command line and runs it.  Returns the positive task ID on
 * success or a negative BRUCE_ERR_* value on failure, exactly like
 * app_runner__run() / app_runner__run_path(). */
int terminal__run_line(const char *line);
