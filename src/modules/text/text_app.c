#include "text_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/clipboard.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/environment.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#define TEXT_MAX_BYTES (32u * 1024u)
#define TEXT_LINE_INPUT_MAX 512u
/* Mirrors the active font's cell size (display__get_font_metrics()) as
 * compile-time constants -- every built-in font is monospace 6x10 today. */
#define TEXT_CHAR_WIDTH 6
#define TEXT_CHAR_HEIGHT 10
#define TEXT_TITLE_HEIGHT 12
#define TEXT_FOOTER_HEIGHT 20
#define TEXT_BAR_HEIGHT 24
#define TEXT_FRAME_MARGIN 2
#define TEXT_TAP_PX 8

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    size_t cursor;
    size_t top_line;
    size_t left_column;
    size_t selection_anchor;
    bool dirty;
    bool read_only;
    bool selection_active;
    /* The finger on the touch screen: where it went down, and how many
     * lines its drag has scrolled so far. */
    int16_t touch_x;
    int16_t touch_y;
    int16_t touch_lines;
    bool touch_down;
    bool touch_moved;
} text_editor_t;

typedef enum {
    TEXT_BUTTON_TYPE,
    TEXT_BUTTON_SAVE,
    TEXT_BUTTON_PASTE,
    TEXT_BUTTON_COPY,
    TEXT_BUTTON_MENU,
    TEXT_BUTTON_CANCEL,
    TEXT_BUTTON_MAX,
} text_button_t;

static const char *const TEXT_BUTTON_LABELS[TEXT_BUTTON_MAX] = {"Type", "Save", "Paste", "Copy", "Menu", "Cancel"};

/* The touch screen's bottom bar. Without a keyboard, Type adds text. */
static size_t text__buttons(const text_editor_t *editor, text_button_t out[TEXT_BUTTON_MAX]) {
    size_t count = 0;
    if (editor->selection_active) {
        out[count++] = TEXT_BUTTON_COPY;
        out[count++] = TEXT_BUTTON_CANCEL;
        return count;
    }
    if (!editor->read_only && !input__has_keyboard()) out[count++] = TEXT_BUTTON_TYPE;
    if (!editor->read_only) out[count++] = TEXT_BUTTON_SAVE;
    if (!editor->read_only) out[count++] = TEXT_BUTTON_PASTE;
    out[count++] = TEXT_BUTTON_COPY;
    out[count++] = TEXT_BUTTON_MENU;
    return count;
}

static int text__footer_height(void) { return input__has_touch() ? TEXT_BAR_HEIGHT : TEXT_FOOTER_HEIGHT; }

static size_t text__visible_lines(void) {
    int lines = (display__height() - TEXT_TITLE_HEIGHT - text__footer_height()) / TEXT_CHAR_HEIGHT;
    return lines > 0 ? (size_t)lines : 1u;
}

static bruce_result_t text__reserve(text_editor_t *editor, size_t required);

static size_t text__utf8_next(const char *text, size_t remaining) {
    unsigned char c = (unsigned char)text[0];
    if (c < 0x80 || remaining < 2) return 1;
    if (c >= 0xC2 && c <= 0xDF && ((unsigned char)text[1] & 0xC0) == 0x80) return 2;
    if (c >= 0xE0 && c <= 0xEF && remaining >= 3 && ((unsigned char)text[1] & 0xC0) == 0x80 &&
        ((unsigned char)text[2] & 0xC0) == 0x80)
        return 3;
    if (c >= 0xF0 && c <= 0xF4 && remaining >= 4 && ((unsigned char)text[1] & 0xC0) == 0x80 &&
        ((unsigned char)text[2] & 0xC0) == 0x80 && ((unsigned char)text[3] & 0xC0) == 0x80)
        return 4;
    return 1;
}

static size_t text__column_to_byte(const text_editor_t *editor, size_t start, size_t end, size_t column) {
    size_t byte = start;
    while (byte < end && column > 0) {
        byte += text__utf8_next(editor->data + byte, end - byte);
        column--;
    }
    return byte;
}

static const char *text__basename(const char *path) {
    if (path == NULL || path[0] == '\0') return "[stdin]";
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bruce_result_t text__load_stdin(size_t size, text_editor_t *editor) {
    if (size > TEXT_MAX_BYTES) return BRUCE_ERR_RESOURCE_LIMIT;
    bruce_result_t result = text__reserve(editor, size + 1u);
    size_t offset = 0;
    while (result == BRUCE_OK && offset < size) {
        size_t read_size = 0;
        result = stdio__read(editor->data + offset, size - offset, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        offset += read_size;
    }
    if (result != BRUCE_OK) return result;
    editor->data[offset] = '\0';
    editor->length = offset;
    editor->dirty = true;
    return BRUCE_OK;
}

static bruce_result_t text__reserve(text_editor_t *editor, size_t required) {
    if (required > TEXT_MAX_BYTES + 1u) return BRUCE_ERR_RESOURCE_LIMIT;
    if (required <= editor->capacity) return BRUCE_OK;

    size_t capacity = editor->capacity > 0 ? editor->capacity : 256u;
    while (capacity < required) {
        size_t next = capacity * 2u;
        capacity = next > TEXT_MAX_BYTES + 1u ? TEXT_MAX_BYTES + 1u : next;
        if (capacity < required && capacity == TEXT_MAX_BYTES + 1u) return BRUCE_ERR_RESOURCE_LIMIT;
    }
    char *data = memory__realloc(editor->data, capacity);
    if (data == NULL) return BRUCE_ERR_NO_MEMORY;
    editor->data = data;
    editor->capacity = capacity;
    return BRUCE_OK;
}

static bruce_result_t text__load(const char *path, text_editor_t *editor) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK && size > TEXT_MAX_BYTES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);
    if (result == BRUCE_OK) result = text__reserve(editor, (size_t)size + 1u);

    size_t offset = 0;
    while (result == BRUCE_OK && offset < (size_t)size) {
        size_t read_size = 0;
        result = storage__read(file, editor->data + offset, (size_t)size - offset, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        offset += read_size;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) return result;

    editor->data[offset] = '\0';
    editor->length = offset;
    return BRUCE_OK;
}

static bruce_result_t text__save(const char *path, text_editor_t *editor) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) return result;

    size_t offset = 0;
    while (result == BRUCE_OK && offset < editor->length) {
        size_t written = 0;
        result = storage__write(file, editor->data + offset, editor->length - offset, &written);
        if (result == BRUCE_OK && written == 0) result = BRUCE_ERR_IO;
        offset += written;
    }
    bruce_result_t close_result = storage__close(file);
    if (result == BRUCE_OK) result = close_result;
    if (result == BRUCE_OK) editor->dirty = false;
    return result;
}

static bruce_result_t text__replace(
    text_editor_t *editor, size_t start, size_t end, const char *replacement, size_t replacement_length
) {
    if (start > end || end > editor->length) return BRUCE_ERR_INVALID_ARGUMENT;
    size_t removed = end - start;
    if (replacement_length > TEXT_MAX_BYTES - (editor->length - removed)) return BRUCE_ERR_RESOURCE_LIMIT;
    size_t new_length = editor->length - removed + replacement_length;
    bruce_result_t result = text__reserve(editor, new_length + 1u);
    if (result != BRUCE_OK) return result;

    memmove(editor->data + start + replacement_length, editor->data + end, editor->length - end + 1u);
    if (replacement_length > 0) memcpy(editor->data + start, replacement, replacement_length);
    editor->length = new_length;
    editor->cursor = start + replacement_length;
    editor->dirty = true;
    return BRUCE_OK;
}

static bruce_result_t text__paste(text_editor_t *editor) {
    if (editor->read_only || clipboard__kind() != BRUCE_CLIPBOARD_TEXT) return BRUCE_OK;
    const char *text = clipboard__get_text();
    return text != NULL ? text__replace(editor, editor->cursor, editor->cursor, text, strlen(text))
                        : BRUCE_OK;
}

static bruce_result_t text__copy_selection(text_editor_t *editor) {
    size_t start = editor->selection_anchor < editor->cursor ? editor->selection_anchor : editor->cursor;
    size_t end = editor->selection_anchor < editor->cursor ? editor->cursor : editor->selection_anchor;
    size_t length = end - start;
    char *text = memory__malloc(length + 1u);
    if (text == NULL) return BRUCE_ERR_NO_MEMORY;
    memcpy(text, editor->data + start, length);
    text[length] = '\0';
    bruce_result_t result = clipboard__set_text(text);
    memory__free(text);
    return result;
}

static size_t text__line_start(const text_editor_t *editor, size_t position) {
    while (position > 0 && editor->data[position - 1u] != '\n') position--;
    return position;
}

static size_t text__line_end(const text_editor_t *editor, size_t position) {
    while (position < editor->length && editor->data[position] != '\n') position++;
    return position;
}

static void text__cursor_location(const text_editor_t *editor, size_t *out_line, size_t *out_column) {
    size_t line = 0;
    size_t column = 0;
    for (size_t i = 0; i < editor->cursor;) {
        if (editor->data[i] == '\n') {
            line++;
            column = 0;
            i++;
        } else {
            i += text__utf8_next(editor->data + i, editor->cursor - i);
            column++;
        }
    }
    *out_line = line;
    *out_column = column;
}

static void text__move_vertical(text_editor_t *editor, int direction) {
    size_t start = text__line_start(editor, editor->cursor);
    size_t unused_line, column;
    text__cursor_location(editor, &unused_line, &column);
    if (direction < 0) {
        if (start == 0) return;
        size_t previous_end = start - 1u;
        size_t previous_start = text__line_start(editor, previous_end);
        size_t previous_columns = 0;
        for (size_t p = previous_start; p < previous_end;) {
            p += text__utf8_next(editor->data + p, previous_end - p);
            previous_columns++;
        }
        editor->cursor = text__column_to_byte(
            editor, previous_start, previous_end, column < previous_columns ? column : previous_columns
        );
    } else {
        size_t end = text__line_end(editor, editor->cursor);
        if (end == editor->length) return;
        size_t next_start = end + 1u;
        size_t next_end = text__line_end(editor, next_start);
        size_t next_columns = 0;
        for (size_t p = next_start; p < next_end;) {
            p += text__utf8_next(editor->data + p, next_end - p);
            next_columns++;
        }
        editor->cursor =
            text__column_to_byte(editor, next_start, next_end, column < next_columns ? column : next_columns);
    }
}

static void text__render(const char *path, text_editor_t *editor) {
    int width = display__width();
    int height = display__height();
    int footer_height = text__footer_height();
    if (width <= 0 || height <= TEXT_TITLE_HEIGHT + footer_height) return;
    uint16_t foreground = config__get_color_primary();
    uint16_t background = config__get_color_background();
    size_t visible_columns = (size_t)(width - 2 * TEXT_FRAME_MARGIN) / TEXT_CHAR_WIDTH;
    size_t visible_lines = text__visible_lines();
    if (visible_columns == 0) visible_columns = 1;

    size_t cursor_line;
    size_t cursor_column;
    text__cursor_location(editor, &cursor_line, &cursor_column);
    if (cursor_line < editor->top_line) editor->top_line = cursor_line;
    if (cursor_line >= editor->top_line + visible_lines) {
        editor->top_line = cursor_line - visible_lines + 1u;
    }
    if (cursor_column < editor->left_column) editor->left_column = cursor_column;
    if (cursor_column >= editor->left_column + visible_columns) {
        editor->left_column = cursor_column - visible_columns + 1u;
    }

    (void)display__begin_frame();
    (void)display__fill_screen(background);
    (void)display__fill_rect(0, 0, (int16_t)width, TEXT_TITLE_HEIGHT, foreground);
    (void)display__set_text_size(1);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(background);
    (void)display__set_cursor(TEXT_FRAME_MARGIN, TEXT_FRAME_MARGIN);
    (void)display__print(text__basename(path));
    if (editor->read_only) (void)display__print(" [RO]");
    if (editor->dirty) (void)display__print(" *");
    if (editor->selection_active) (void)display__print(" [COPY]");

    size_t position = 0;
    size_t line = 0;
    while (line < editor->top_line && position < editor->length) {
        if (editor->data[position++] == '\n') line++;
    }
    for (size_t row = 0; row < visible_lines; ++row) {
        size_t end = text__line_end(editor, position);
        size_t line_length = 0;
        for (size_t p = position; p < end;) {
            p += text__utf8_next(editor->data + p, end - p);
            line_length++;
        }
        char visible[64];
        size_t count = 0;
        size_t visible_start = position;
        size_t visible_end = position;
        if (editor->left_column < line_length) {
            count = line_length - editor->left_column;
            if (count > visible_columns) count = visible_columns;
            if (count >= sizeof(visible)) count = sizeof(visible) - 1u;
            size_t visible_position = text__column_to_byte(editor, position, end, editor->left_column);
            visible_start = visible_position;
            size_t written = 0;
            for (size_t i = 0; i < count && visible_position < end; ++i) {
                size_t bytes = text__utf8_next(editor->data + visible_position, end - visible_position);
                if (written + bytes >= sizeof(visible)) break;
                if (bytes == 1 && editor->data[visible_position] == '\t') {
                    /* display__print() treats any byte < 0x20 as a zero-width
                     * no-op (see display_text.c), which would silently collapse
                     * this column and shift every later glyph on the line one
                     * cell to the left of where the cursor/selection-highlight
                     * math below (which counts this byte as one column) expects
                     * it. Draw a blank cell instead so the drawn width matches. */
                    visible[written] = ' ';
                } else {
                    memcpy(visible + written, editor->data + visible_position, bytes);
                }
                written += bytes;
                visible_position += bytes;
            }
            count = written;
            visible_end = visible_position;
        }
        visible[count] = '\0';
        (void)display__set_text_color(foreground);
        (void)display__set_cursor(TEXT_FRAME_MARGIN, (int16_t)(TEXT_TITLE_HEIGHT + row * TEXT_CHAR_HEIGHT));
        (void)display__print(visible);
        if (editor->selection_active) {
            size_t selection_start =
                editor->selection_anchor < editor->cursor ? editor->selection_anchor : editor->cursor;
            size_t selection_end =
                editor->selection_anchor < editor->cursor ? editor->cursor : editor->selection_anchor;
            if (selection_start < visible_end && selection_end > visible_start) {
                size_t highlighted_start = selection_start > visible_start ? selection_start : visible_start;
                size_t highlighted_end = selection_end < visible_end ? selection_end : visible_end;
                size_t before_columns = 0;
                size_t selected_columns = 0;
                for (size_t p = visible_start; p < highlighted_start;) {
                    p += text__utf8_next(editor->data + p, highlighted_start - p);
                    before_columns++;
                }
                for (size_t p = highlighted_start; p < highlighted_end;) {
                    p += text__utf8_next(editor->data + p, highlighted_end - p);
                    selected_columns++;
                }
                size_t start_offset = highlighted_start - visible_start;
                size_t end_offset = highlighted_end - visible_start;
                char saved = visible[end_offset];
                visible[end_offset] = '\0';
                int selected_x = TEXT_FRAME_MARGIN + (int)before_columns * TEXT_CHAR_WIDTH;
                int selected_y = TEXT_TITLE_HEIGHT + (int)row * TEXT_CHAR_HEIGHT;
                (void)display__fill_rect(
                    (int16_t)selected_x,
                    (int16_t)selected_y,
                    (int16_t)(selected_columns * TEXT_CHAR_WIDTH),
                    TEXT_CHAR_HEIGHT,
                    foreground
                );
                (void)display__set_text_color(background);
                (void)display__set_cursor((int16_t)selected_x, (int16_t)selected_y);
                (void)display__print(visible + start_offset);
                visible[end_offset] = saved;
            }
        }
        if (end == editor->length) break;
        position = end + 1u;
    }

    int cursor_x = TEXT_FRAME_MARGIN + (int)(cursor_column - editor->left_column) * TEXT_CHAR_WIDTH;
    int cursor_y = TEXT_TITLE_HEIGHT + (int)(cursor_line - editor->top_line) * TEXT_CHAR_HEIGHT;
    (void)display__fill_rect(
        (int16_t)cursor_x, (int16_t)(cursor_y + TEXT_CHAR_HEIGHT - 1), TEXT_CHAR_WIDTH - 1, 1, foreground
    );

    char status[32];
    snprintf(
        status, sizeof(status), "Ln %u Col %u", (unsigned)(cursor_line + 1u), (unsigned)(cursor_column + 1u)
    );
    if (input__has_touch()) {
        /* The bar replaces the footer; the title shows where the cursor is. */
        const char *right = editor->selection_active ? "Tap the end, then Copy" : status;
        (void)display__fill_rect(
            (int16_t)(width - (int)strlen(right) * TEXT_CHAR_WIDTH - 2 * TEXT_FRAME_MARGIN), 0,
            (int16_t)((int)strlen(right) * TEXT_CHAR_WIDTH + 2 * TEXT_FRAME_MARGIN), TEXT_TITLE_HEIGHT, foreground
        );
        (void)display__set_text_color(background);
        (void)display__set_cursor(
            (int16_t)(width - (int)strlen(right) * TEXT_CHAR_WIDTH - TEXT_FRAME_MARGIN), TEXT_FRAME_MARGIN
        );
        (void)display__print(right);

        text_button_t buttons[TEXT_BUTTON_MAX];
        size_t count = text__buttons(editor, buttons);
        int y = height - TEXT_BAR_HEIGHT;
        (void)display__fill_rect(0, (int16_t)y, (int16_t)width, TEXT_BAR_HEIGHT, background);
        for (size_t i = 0; i < count; ++i) {
            int x = width * (int)i / (int)count;
            int w = width * (int)(i + 1u) / (int)count - x;
            const char *label = TEXT_BUTTON_LABELS[buttons[i]];
            (void)display__fill_rect(
                (int16_t)(x + 1), (int16_t)(y + 2), (int16_t)(w - 2), TEXT_BAR_HEIGHT - 2, foreground
            );
            (void)display__set_cursor(
                (int16_t)(x + (w - (int)strlen(label) * TEXT_CHAR_WIDTH) / 2),
                (int16_t)(y + 2 + (TEXT_BAR_HEIGHT - 2 - TEXT_CHAR_HEIGHT) / 2)
            );
            (void)display__print(label);
        }
        (void)display__present();
        return;
    }
    (void)display__draw_line(
        0,
        (int16_t)(height - TEXT_FOOTER_HEIGHT),
        (int16_t)(width - 1),
        (int16_t)(height - TEXT_FOOTER_HEIGHT),
        foreground
    );
    (void)display__set_text_color(BRUCE_COLOR_LIGHTGREY);
    (void)display__set_cursor(TEXT_FRAME_MARGIN, (int16_t)(height - TEXT_FOOTER_HEIGHT + 2));
    (void)display__print(status);
    (void)display__set_text_color(foreground);
    (void)display__set_cursor(TEXT_FRAME_MARGIN, (int16_t)(height - TEXT_CHAR_HEIGHT));
    if (editor->selection_active) {
        (void)display__print("Select: Copy  Back: Cancel");
    } else {
        (void)display__print(editor->read_only ? "^X Exit  RO" : "^S Save  ^X Exit");
    }
    (void)display__present();
}

static bruce_result_t text__edit_line(text_editor_t *editor) {
    size_t start = text__line_start(editor, editor->cursor);
    size_t end = text__line_end(editor, editor->cursor);
    char current[TEXT_LINE_INPUT_MAX];
    size_t length = end - start;
    if (length >= sizeof(current)) return BRUCE_ERR_RESOURCE_LIMIT;
    memcpy(current, editor->data + start, length);
    current[length] = '\0';

    char edited[TEXT_LINE_INPUT_MAX];
    bruce_result_t result =
        dialog__text_input("Text editor", "Edit current line", current, false, edited, sizeof(edited));
    if (result != BRUCE_OK) return result;
    return text__replace(editor, start, end, edited, strlen(edited));
}

static bruce_result_t text__delete_line(text_editor_t *editor) {
    size_t start = text__line_start(editor, editor->cursor);
    size_t end = text__line_end(editor, editor->cursor);
    if (end < editor->length) end++;
    else if (start > 0) start--;
    bruce_result_t result = text__replace(editor, start, end, NULL, 0);
    if (result == BRUCE_OK) editor->cursor = text__line_start(editor, editor->cursor);
    return result;
}

static bruce_result_t text__show_error(const char *action, bruce_result_t result) {
    char message[64];
    snprintf(message, sizeof(message), "%s failed: %s", action, result__to_string(result));
    (void)dialog__message(BRUCE_DIALOG_ERROR, "Text editor", message);
    return result;
}

static bruce_result_t text__ensure_save_path(char *path, size_t path_size) {
    if (path[0] != '\0') return BRUCE_OK;
    bruce_result_t result =
        dialog__text_input("Save piped text", "Absolute path", "/untitled.txt", false, path, path_size);
    if (result != BRUCE_OK) return result;
    return path[0] == '/' ? BRUCE_OK : BRUCE_ERR_INVALID_PATH;
}

static bruce_result_t text__save_to_path(char *path, size_t path_size, text_editor_t *editor) {
    bruce_result_t result = text__ensure_save_path(path, path_size);
    return result == BRUCE_OK ? text__save(path, editor) : result;
}

static bruce_result_t text__exit_prompt(char *path, size_t path_size, text_editor_t *editor, bool *out_exit) {
    *out_exit = false;
    if (!editor->dirty) {
        *out_exit = true;
        return BRUCE_OK;
    }
    const bruce_dialog_choice_t choices[] = {
        {.label = "Save and exit",   .value = "save"   },
        {.label = "Discard changes", .value = "discard"},
        {.label = "Cancel",          .value = "cancel" },
    };
    size_t selected = 0;
    bruce_result_t result = dialog__choice(
        "Unsaved changes", text__basename(path), choices, sizeof(choices) / sizeof(choices[0]), &selected
    );
    if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (result != BRUCE_OK) return result;
    if (strcmp(choices[selected].value, "cancel") == 0) return BRUCE_OK;
    if (strcmp(choices[selected].value, "save") == 0) {
        result = text__save_to_path(path, path_size, editor);
        if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
        if (result != BRUCE_OK) return text__show_error("Save", result);
    }
    *out_exit = true;
    return BRUCE_OK;
}

static bruce_result_t text__actions(char *path, size_t path_size, text_editor_t *editor, bool *out_exit) {
    bruce_dialog_choice_t choices[8];
    size_t count = 0;
    if (!editor->read_only) {
        choices[count++] = (bruce_dialog_choice_t){.label = "Edit line", .value = "edit"};
        choices[count++] = (bruce_dialog_choice_t){.label = "Delete line", .value = "delete"};
    }
    choices[count++] = (bruce_dialog_choice_t){.label = "Copy", .value = "copy"};
    if (!editor->read_only) {
        choices[count++] = (bruce_dialog_choice_t){.label = "Paste", .value = "paste"};
        choices[count++] = (bruce_dialog_choice_t){.label = "Save", .value = "save"};
    }
    choices[count++] = (bruce_dialog_choice_t){.label = "Exit", .value = "exit"};
    choices[count++] = (bruce_dialog_choice_t){.label = "Cancel", .value = "cancel"};

    size_t selected = 0;
    *out_exit = false;
    char title[BRUCE_STORAGE_PATH_MAX + 16u];
    snprintf(title, sizeof(title), "Text editor - %s", text__basename(path));
    bruce_result_t result = dialog__choice(title, NULL, choices, count, &selected);
    if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (result != BRUCE_OK) return result;
    const char *action = choices[selected].value;
    if (strcmp(action, "cancel") == 0) return BRUCE_OK;
    if (strcmp(action, "edit") == 0) {
        result = text__edit_line(editor);
        return result == BRUCE_ERR_CANCELLED ? BRUCE_OK : result;
    }
    if (strcmp(action, "delete") == 0) return text__delete_line(editor);
    if (strcmp(action, "copy") == 0) {
        editor->selection_anchor = editor->cursor;
        editor->selection_active = true;
        return BRUCE_OK;
    }
    if (strcmp(action, "paste") == 0) return text__paste(editor);
    if (strcmp(action, "save") == 0) {
        result = text__save_to_path(path, path_size, editor);
        if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
        if (result == BRUCE_OK) (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Text editor", "File saved");
        return result == BRUCE_OK ? BRUCE_OK : text__show_error("Save", result);
    }
    return text__exit_prompt(path, path_size, editor, out_exit);
}

static size_t text__line_position(const text_editor_t *editor, size_t line) {
    size_t position = 0;
    while (line > 0 && position < editor->length) {
        if (editor->data[position++] == '\n') line--;
    }
    return position;
}

static size_t text__line_count(const text_editor_t *editor) {
    size_t count = 1;
    for (size_t i = 0; i < editor->length; ++i) count += editor->data[i] == '\n';
    return count;
}

/* Moves the cursor to `column` of `line`, or the line's end. */
static void text__move_to(text_editor_t *editor, size_t line, size_t column) {
    size_t start = text__line_position(editor, line);
    editor->cursor = text__column_to_byte(editor, start, text__line_end(editor, start), column);
}

/* Scrolls the view by `lines`, taking the cursor along when it would leave it. */
static void text__scroll(text_editor_t *editor, int lines) {
    size_t visible = text__visible_lines();
    size_t total = text__line_count(editor);
    size_t max_top = total > visible ? total - visible : 0;
    long top = (long)editor->top_line + lines;
    if (top < 0) top = 0;
    if ((size_t)top > max_top) top = (long)max_top;
    editor->top_line = (size_t)top;
    size_t line, column;
    text__cursor_location(editor, &line, &column);
    if (line < editor->top_line) text__move_to(editor, editor->top_line, column);
    if (line >= editor->top_line + visible) text__move_to(editor, editor->top_line + visible - 1u, column);
}

static bruce_result_t text__type(text_editor_t *editor) {
    char *typed = memory__malloc(TEXT_LINE_INPUT_MAX);
    if (typed == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = dialog__text_input("Text editor", "Add text", NULL, false, typed, TEXT_LINE_INPUT_MAX);
    if (result == BRUCE_OK) result = text__replace(editor, editor->cursor, editor->cursor, typed, strlen(typed));
    memory__free(typed);
    return result == BRUCE_ERR_CANCELLED ? BRUCE_OK : result;
}

static bruce_result_t text__save_now(char *path, size_t path_size, text_editor_t *editor) {
    if (editor->read_only) return BRUCE_OK;
    bruce_result_t result = text__save_to_path(path, path_size, editor);
    if (result != BRUCE_OK && result != BRUCE_ERR_CANCELLED) (void)text__show_error("Save", result);
    return BRUCE_OK;
}

static bruce_result_t
text__press_button(char *path, size_t path_size, text_editor_t *editor, text_button_t button, bool *out_exit) {
    switch (button) {
        case TEXT_BUTTON_TYPE: return text__type(editor);
        case TEXT_BUTTON_SAVE: return text__save_now(path, path_size, editor);
        case TEXT_BUTTON_PASTE: return text__paste(editor);
        case TEXT_BUTTON_COPY:
            if (editor->selection_active) {
                editor->selection_active = false;
                return text__copy_selection(editor);
            }
            editor->selection_anchor = editor->cursor;
            editor->selection_active = true;
            return BRUCE_OK;
        case TEXT_BUTTON_CANCEL: editor->selection_active = false; return BRUCE_OK;
        case TEXT_BUTTON_MENU: return text__actions(path, path_size, editor, out_exit);
        default: return BRUCE_OK;
    }
}

/* A drag scrolls the text, a tap puts the cursor there, and the bar's
 * buttons act when released over the one pressed. Sets `out_changed` when
 * the screen needs drawing again. */
static bruce_result_t text__touch(
    char *path, size_t path_size, text_editor_t *editor, const bruce_input_event_t *event, bool *out_exit,
    bool *out_changed
) {
    int bar_y = display__height() - TEXT_BAR_HEIGHT;
    *out_changed = false;
    if (event->action == BRUCE_INPUT_PRESS) {
        editor->touch_x = event->x;
        editor->touch_y = event->y;
        editor->touch_lines = 0;
        editor->touch_down = true;
        editor->touch_moved = false;
        return BRUCE_OK;
    }
    if (!editor->touch_down) return BRUCE_OK;
    if (event->action == BRUCE_INPUT_RELEASE) editor->touch_down = false;

    if (editor->touch_y >= bar_y) {
        if (event->action != BRUCE_INPUT_RELEASE || event->y < bar_y) return BRUCE_OK;
        text_button_t buttons[TEXT_BUTTON_MAX];
        size_t count = text__buttons(editor, buttons);
        int width = display__width();
        int pressed = editor->touch_x * (int)count / width;
        if (pressed < 0 || pressed >= (int)count || event->x * (int)count / width != pressed) return BRUCE_OK;
        *out_changed = true;
        return text__press_button(path, path_size, editor, buttons[pressed], out_exit);
    }

    int dy = editor->touch_y - event->y;
    if (abs(dy) > TEXT_TAP_PX || abs(event->x - editor->touch_x) > TEXT_TAP_PX) editor->touch_moved = true;
    int lines = dy / TEXT_CHAR_HEIGHT;
    /* E-paper scrolls once the finger lifts: following it would refresh the
     * panel over and over while the text keeps moving. */
    bool follow = event->action == BRUCE_INPUT_RELEASE || !display__is_epaper();
    if (follow && lines != editor->touch_lines) {
        text__scroll(editor, lines - editor->touch_lines);
        editor->touch_lines = (int16_t)lines;
        *out_changed = true;
    }
    if (event->action == BRUCE_INPUT_RELEASE && !editor->touch_moved && event->y >= TEXT_TITLE_HEIGHT) {
        int column = (event->x - TEXT_FRAME_MARGIN + TEXT_CHAR_WIDTH / 2) / TEXT_CHAR_WIDTH;
        size_t line = editor->top_line + (size_t)((event->y - TEXT_TITLE_HEIGHT) / TEXT_CHAR_HEIGHT);
        size_t total = text__line_count(editor);
        if (line >= total) line = total - 1u;
        text__move_to(editor, line, editor->left_column + (size_t)(column > 0 ? column : 0));
        *out_changed = true;
    }
    return BRUCE_OK;
}

static bruce_result_t text__run_editor(char *path, size_t path_size, text_editor_t *editor) {
    (void)input__flush();
    bool redraw = true;
    for (;;) {
        if (redraw) text__render(path, editor);
        redraw = true;
        bruce_input_event_t event;
        bruce_result_t result = input__read(&event, UINT32_MAX);
        if (result == BRUCE_ERR_NOT_FOREGROUND) {
            if (display__await_redraw()) continue;
            return BRUCE_OK;
        }
        bool exit_editor = false;
        if (result == BRUCE_OK && event.type == BRUCE_INPUT_TOUCH) {
            result = text__touch(path, path_size, editor, &event, &exit_editor, &redraw);
            if (result != BRUCE_OK) (void)text__show_error("Edit", result);
            if (exit_editor) return BRUCE_OK;
            continue;
        }
        if (result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) {
            redraw = false;
            continue;
        }

        bool semantic = event.type != BRUCE_INPUT_KEY || event.value != event.code;
        if (!editor->selection_active && !semantic && event.code == 0x13) {
            result = text__save_now(path, path_size, editor);
        } else if (!editor->selection_active && !semantic && event.code == 0x18) {
            result = text__exit_prompt(path, path_size, editor, &exit_editor);
        } else if (!editor->selection_active && !semantic && event.code == 0x03) {
            /* Ctrl+C copies the whole document outside selection mode. */
            result = clipboard__set_text(editor->data);
        } else if (!editor->selection_active && !semantic && event.code == 0x16) {
            result = text__paste(editor);
        } else if (
            editor->selection_active &&
            (event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A)
        ) {
            result = text__copy_selection(editor);
            editor->selection_active = false;
        } else if (semantic && (event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_PREV)) {
            text__move_vertical(editor, -1);
        } else if (semantic && (event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT)) {
            text__move_vertical(editor, 1);
        } else if (semantic && event.code == BRUCE_INPUT_CODE_LEFT) {
            if (editor->cursor > 0) {
                editor->cursor--;
                while (editor->cursor > 0 && ((unsigned char)editor->data[editor->cursor] & 0xC0) == 0x80)
                    editor->cursor--;
            }
        } else if (semantic && event.code == BRUCE_INPUT_CODE_RIGHT) {
            if (editor->cursor < editor->length)
                editor->cursor +=
                    text__utf8_next(editor->data + editor->cursor, editor->length - editor->cursor);
        } else if (semantic && event.code == BRUCE_INPUT_CODE_HOME) {
            editor->cursor = text__line_start(editor, editor->cursor);
        } else if (
            !editor->selection_active && !editor->read_only && semantic &&
            event.code == BRUCE_INPUT_CODE_DELETE
        ) {
            if (editor->cursor < editor->length) {
                size_t next = editor->cursor +
                              text__utf8_next(editor->data + editor->cursor, editor->length - editor->cursor);
                result = text__replace(editor, editor->cursor, next, NULL, 0);
            }
        } else if (
            semantic && (event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A ||
                         event.code == BRUCE_INPUT_CODE_MENU || event.code == BRUCE_INPUT_CODE_BACK ||
                         event.code == BRUCE_INPUT_CODE_BUTTON_B)
        ) {
            if (editor->selection_active) {
                editor->selection_active = false;
            } else {
                result = text__actions(path, path_size, editor, &exit_editor);
            }
        } else if (
            !editor->selection_active && !editor->read_only && !semantic &&
            (event.code == '\b' || event.code == 0x7f)
        ) {
            if (editor->cursor > 0) {
                size_t previous = editor->cursor - 1u;
                while (previous > 0 && ((unsigned char)editor->data[previous] & 0xC0) == 0x80) previous--;
                result = text__replace(editor, previous, editor->cursor, NULL, 0);
            }
        } else if (
            !editor->selection_active && !editor->read_only && !semantic &&
            (event.code == '\n' || event.code == '\r')
        ) {
            result = text__replace(editor, editor->cursor, editor->cursor, "\n", 1u);
        } else if (
            !editor->selection_active && !editor->read_only && !semantic && event.code >= 0x20 &&
            event.code <= 0x7e
        ) {
            char character = (char)event.code;
            result = text__replace(editor, editor->cursor, editor->cursor, &character, 1u);
        }
        if (result != BRUCE_OK) (void)text__show_error("Edit", result);
        if (exit_editor) return BRUCE_OK;
    }
}

int text_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Edit a file, or piped stdin.");
    ap_add_flag(parser, "r");
    ap_set_opt_help(parser, "r", "Alias for --read-only");
    ap_add_flag(parser, "read-only");
    ap_set_opt_help(parser, "read-only", "View without allowing edits or saves");
    ap_add_optional_arg(parser, "path", "Path to a text file or save destination for piped input");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    const char *path_arg = ap_get_arg(parser, "path");
    bool read_only = ap_found(parser, "r") || ap_found(parser, "read-only");
    const char *stdin_size_arg = environment__get("STDIN_SIZE");
    char *end = NULL;
    unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
    bool stdin_requested = stdin_size_arg != NULL;
    bool from_stdin = stdin_size_arg != NULL && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0' &&
                      parsed_stdin_size <= TEXT_MAX_BYTES;
    char path[BRUCE_STORAGE_PATH_MAX] = {0};
    int path_length = path_arg != NULL ? snprintf(path, sizeof(path), "%s", path_arg) : 0;
    ap_free(parser);
    if ((stdin_requested && !from_stdin) || path_length < 0 || (size_t)path_length >= sizeof(path) ||
        (!from_stdin && path[0] == '\0')) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    text_editor_t editor = {.read_only = read_only};
    bruce_result_t result =
        from_stdin ? text__load_stdin((size_t)parsed_stdin_size, &editor) : text__load(path, &editor);
    if (read_only) editor.dirty = false;
    if (result != BRUCE_OK) {
        (void)text__show_error(result == BRUCE_ERR_RESOURCE_LIMIT ? "File too large" : "Open", result);
        memory__free(editor.data);
        return result;
    }
    result = text__run_editor(path, sizeof(path), &editor);
    memory__free(editor.data);
    return result;
}
