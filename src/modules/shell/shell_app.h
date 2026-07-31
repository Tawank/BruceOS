#pragma once

#include <stdbool.h>

int shell_app_main(int argc, char **argv);
int shell_loader__run_path(const char *path, const char *arg, bool in_background);
