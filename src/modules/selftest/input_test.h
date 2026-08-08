#pragma once

#include <stdbool.h>

bool selftest__run_input_poll_case(void);
bool selftest__run_input_inject_case(void);
bool selftest__run_input_flush_case(void);
bool selftest__run_input_non_blocking_case(void);
bool selftest__run_input_peek_case(void);
bool selftest__run_input_wait_case(void);
bool selftest__run_input_check_case(void);
bool selftest__run_input_hotkey_duration_case(void);
bool selftest__run_input_hotkey_code_name_case(void);
bool selftest__run_input_hotkey_find_case(void);
bool selftest__run_input_hotkey_emit_case(void);
