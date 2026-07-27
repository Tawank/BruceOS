#pragma once

#include <stdbool.h>

int serial_commands_app_main(int argc, char **argv);
int serial_commands__run_line(const char *line, bool in_background);
