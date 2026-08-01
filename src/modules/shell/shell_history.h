#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "shell_line_editor.h"

#define SHELL_HISTORY_PATH "/shell_history"

bruce_result_t shell_history__append(const char *path, const char *line);
bruce_result_t shell_history__previous(
    const char *path, uint64_t before, char *line, size_t capacity, uint64_t *out_start
);
bruce_result_t shell_history__next(
    const char *path, uint64_t current_start, char *line, size_t capacity, uint64_t *out_start
);

typedef struct {
    char *draft;
    size_t capacity;
    uint64_t offset;
    bool browsing;
} shell_history_browser_t;

void shell_history_browser__init(shell_history_browser_t *browser, char *draft, size_t capacity);
bool shell_history_browser__previous(shell_history_browser_t *browser, shell_line_editor_t *editor);
bool shell_history_browser__next(shell_history_browser_t *browser, shell_line_editor_t *editor);
void shell_history_browser__reset(shell_history_browser_t *browser);
