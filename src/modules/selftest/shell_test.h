#pragma once

#include <stdbool.h>

bool selftest__run_shell_language_case(void);
bool selftest__run_shell_script_case(void);
bool selftest__run_shell_control_flow_case(void);
bool selftest__run_shell_local_case(void);
bool selftest__run_shell_command_substitution_case(void);
bool selftest__run_shell_arith_word_case(void);
bool selftest__run_shell_output_redirect_case(void);
bool selftest__run_shell_builtin_redirect_case(void);
bool selftest__run_shell_input_redirect_case(void);
bool selftest__run_shell_arith_redirect_case(void);
bool selftest__run_shell_heredoc_case(void);
bool selftest__run_shell_cat_interactive_case(void);
bool selftest__run_shell_multiline_case(void);
bool selftest__run_shell_loops_case(void);
bool selftest__run_shell_case_case(void);
bool selftest__run_shell_glob_case(void);
bool selftest__run_shell_brace_case(void);
bool selftest__run_shell_pipe_redirect_case(void);
bool selftest__run_shell_bnu_text_pipe_case(void);
bool selftest__run_shell_read_case(void);
bool selftest__run_shell_stdio_inheritance_case(void);
bool selftest__run_shell_tty_size_case(void);
bool selftest__run_shell_interrupt_case(void);
bool selftest__run_shell_eof_case(void);
