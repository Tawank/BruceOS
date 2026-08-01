#pragma once

#include <stdbool.h>
#include <stdint.h>

int serial_commands_app_main(int argc, char **argv);
int serial_commands__run_line(const char *line, bool in_background);
bool serial_commands__wait_ready(uint32_t timeout_ms);
