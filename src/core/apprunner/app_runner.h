#pragma once
#include <stdbool.h>

void app_runner__register_defaults(void);
int app_runner__run(const char *app_name, bool in_background);
