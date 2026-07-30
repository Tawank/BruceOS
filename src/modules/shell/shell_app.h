#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SHELL__MAX_VARIABLES 24
#define SHELL__VARIABLE_NAME_MAX 32
#define SHELL__VARIABLE_VALUE_MAX 128
#define SHELL__LINE_MAX 512

typedef struct {
    bool in_use;
    char name[SHELL__VARIABLE_NAME_MAX];
    char value[SHELL__VARIABLE_VALUE_MAX];
} shell_variable_t;

typedef struct {
    shell_variable_t variables[SHELL__MAX_VARIABLES];
    int last_status;
    bool exit_requested;
    int exit_status;
} shell_state_t;

void shell__state_init(shell_state_t *state);
int shell__execute_line(shell_state_t *state, const char *line);
int shell_app_main(int argc, char **argv);
int shell_loader__run_path(const char *path, const char *arg, bool in_background);
