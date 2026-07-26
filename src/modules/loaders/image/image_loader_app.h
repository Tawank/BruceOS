#pragma once

#include <stdbool.h>

int image_app_main(int argc, char **argv);
int image_viewer_app_main(int argc, char **argv);
int image_loader__run_path(const char *path, const char *arg, bool in_background);
