#include "shell_line_editor.h"

#include <stdbool.h>
#include <string.h>

void shell_line_editor__init(shell_line_editor_t *editor, char *text, size_t capacity) {
    editor->text = text;
    editor->capacity = capacity;
    editor->length = 0;
    editor->cursor = 0;
    if (capacity > 0) text[0] = '\0';
}

bool shell_line_editor__insert(shell_line_editor_t *editor, char c) {
    if (editor->length + 1 >= editor->capacity) return false;
    memmove(editor->text + editor->cursor + 1, editor->text + editor->cursor, editor->length - editor->cursor + 1);
    editor->text[editor->cursor++] = c;
    editor->length++;
    return true;
}

bool shell_line_editor__backspace(shell_line_editor_t *editor) {
    if (editor->cursor == 0) return false;
    memmove(
        editor->text + editor->cursor - 1,
        editor->text + editor->cursor,
        editor->length - editor->cursor + 1
    );
    editor->cursor--;
    editor->length--;
    return true;
}

bool shell_line_editor__delete(shell_line_editor_t *editor) {
    if (editor->cursor >= editor->length) return false;
    memmove(
        editor->text + editor->cursor,
        editor->text + editor->cursor + 1,
        editor->length - editor->cursor
    );
    editor->length--;
    return true;
}

bool shell_line_editor__left(shell_line_editor_t *editor) {
    if (editor->cursor == 0) return false;
    editor->cursor--;
    return true;
}

bool shell_line_editor__right(shell_line_editor_t *editor) {
    if (editor->cursor >= editor->length) return false;
    editor->cursor++;
    return true;
}

void shell_line_editor__set(shell_line_editor_t *editor, const char *text) {
    size_t length = strlen(text);
    if (length >= editor->capacity) length = editor->capacity - 1;
    memcpy(editor->text, text, length);
    editor->text[length] = '\0';
    editor->length = length;
    editor->cursor = length;
}
