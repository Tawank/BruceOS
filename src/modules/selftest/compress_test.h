#pragma once

#include <stdbool.h>

bool selftest__run_compress_one_shot_case(void);
bool selftest__run_compress_streaming_case(void);
bool selftest__run_compress_corrupt_input_case(void);
bool selftest__run_compress_invalid_args_case(void);
