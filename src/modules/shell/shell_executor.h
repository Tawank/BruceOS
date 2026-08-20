#pragma once

#include "shell_internal.h"
#include "shell_parser.h"

int shell_executor__plan(shell_state_t *state, const shell_plan_t *plan);
int shell_executor__page_help(void);
