#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *text;
    size_t capacity;
    size_t length;
    size_t cursor;
} shell_line_editor_t;

void shell_line_editor__init(shell_line_editor_t *editor, char *text, size_t capacity);
bool shell_line_editor__insert(shell_line_editor_t *editor, char c);
bool shell_line_editor__backspace(shell_line_editor_t *editor);
bool shell_line_editor__delete(shell_line_editor_t *editor);
bool shell_line_editor__left(shell_line_editor_t *editor);
bool shell_line_editor__right(shell_line_editor_t *editor);
void shell_line_editor__set(shell_line_editor_t *editor, const char *text);
