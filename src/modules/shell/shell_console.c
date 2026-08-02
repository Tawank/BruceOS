#include "shell_console.h"

#include <stdio.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "shell_history.h"
#include "shell_internal.h"
#include "shell_line_editor.h"

#define SHELL_CONSOLE_ESCAPE_MAX 8
#define SHELL_CONSOLE_ESCAPE_TIMEOUT_MS 50

#define SHELL_CONSOLE_CTRL_A 0x01
#define SHELL_CONSOLE_CTRL_E 0x05
#define SHELL_CONSOLE_CTRL_U 0x15
#define SHELL_CONSOLE_ESCAPE 0x1b
#define SHELL_CONSOLE_DELETE 0x7f

static const char SHELL_CONSOLE_PROMPT[] = "\r\033[2K\033[1;36mbruce\033[0m$ ";
static volatile bool s_shell_console_ready;

static void shell_console__redraw(const shell_line_editor_t *editor) {
    (void)stdio__write(SHELL_CONSOLE_PROMPT, sizeof(SHELL_CONSOLE_PROMPT) - 1);
    (void)stdio__write(editor->text, editor->length);
    if (editor->cursor < editor->length) {
        stdio__printf("\033[%uD", (unsigned)(editor->length - editor->cursor));
    }
}

static int shell_console__read_byte(uint32_t timeout_ms) {
    unsigned char byte;
    size_t size = 0;
    bruce_result_t result = stdio__read(&byte, 1, timeout_ms, &size);
    return result == BRUCE_OK && size == 1 ? byte : result;
}

static size_t shell_console__read_escape(unsigned char *sequence, size_t capacity) {
    size_t used = 0;
    while (used < capacity) {
        int input = shell_console__read_byte(SHELL_CONSOLE_ESCAPE_TIMEOUT_MS);
        if (input < 0) break;
        unsigned char byte = (unsigned char)input;
        sequence[used++] = byte;
        if (used > 1 && byte >= 0x40 && byte <= 0x7e) break;
    }
    return used;
}

static bool shell_console__handle_escape(
    shell_line_editor_t *editor, shell_history_browser_t *history
) {
    unsigned char sequence[SHELL_CONSOLE_ESCAPE_MAX] = {0};
    size_t size = shell_console__read_escape(sequence, sizeof(sequence));
    if (size < 2 || sequence[0] != '[') return false;

    switch (sequence[size - 1]) {
        case 'A': return shell_history_browser__previous(history, editor);
        case 'B': return shell_history_browser__next(history, editor);
        case 'C': return shell_line_editor__right(editor);
        case 'D': return shell_line_editor__left(editor);
        case 'H':
            if (editor->cursor == 0) return false;
            editor->cursor = 0;
            return true;
        case 'F':
            if (editor->cursor == editor->length) return false;
            editor->cursor = editor->length;
            return true;
        case '~':
            return size == 3 && sequence[1] == '3' && shell_line_editor__delete(editor);
        default: return false;
    }
}

static bool shell_console__handle_byte(
    shell_line_editor_t *editor, shell_history_browser_t *history, unsigned char byte
) {
    switch (byte) {
        case '\b':
        case SHELL_CONSOLE_DELETE: return shell_line_editor__backspace(editor);
        case SHELL_CONSOLE_CTRL_A:
            if (editor->cursor == 0) return false;
            editor->cursor = 0;
            return true;
        case SHELL_CONSOLE_CTRL_E:
            if (editor->cursor == editor->length) return false;
            editor->cursor = editor->length;
            return true;
        case SHELL_CONSOLE_CTRL_U:
            if (editor->length == 0) return false;
            shell_line_editor__set(editor, "");
            return true;
        case SHELL_CONSOLE_ESCAPE: return shell_console__handle_escape(editor, history);
        default:
            if (byte < ' ' || byte > '~') return false;
            shell_history_browser__reset(history);
            return shell_line_editor__insert(editor, (char)byte);
    }
}

int shell_console__read_line(char *line, size_t capacity, bool *skip_lf) {
    shell_line_editor_t editor;
    shell_line_editor__init(&editor, line, capacity);
    char draft[SHELL__LINE_MAX] = {0};
    shell_history_browser_t history;
    shell_history_browser__init(&history, draft, sizeof(draft));
    shell_console__redraw(&editor);
    s_shell_console_ready = true;

    for (;;) {
        int input = shell_console__read_byte(UINT32_MAX);
        if (input < 0) return input;
        unsigned char byte = (unsigned char)input;
        if (*skip_lf && byte == '\n') {
            *skip_lf = false;
            continue;
        }
        *skip_lf = false;
        if (byte == '\r' || byte == '\n') {
            *skip_lf = byte == '\r';
            (void)stdio__write("\r\n", 2);
            return (int)editor.length;
        }
        if (shell_console__handle_byte(&editor, &history, byte)) shell_console__redraw(&editor);
    }
}

void shell_console__reset_ready(void) { s_shell_console_ready = false; }

bool shell_console__is_ready(void) { return s_shell_console_ready; }
