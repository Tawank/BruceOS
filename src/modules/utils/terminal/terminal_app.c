#include "terminal_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "modules/shell/shell_history.h"
#include "modules/shell/shell_line_editor.h"
#include "terminal_ansi.h"

#define TERMINAL__TRANSCRIPT_CAPACITY 4096
#define TERMINAL__LINE_CAPACITY 512
#define TERMINAL__CHAR_W 6
#define TERMINAL__CHAR_H 8
#define TERMINAL__TITLE_H 12
#define TERMINAL__MAX_VISIBLE_ROWS 40
#define TERMINAL__FRAME_MARGIN 2
#define TERMINAL__PROMPT_COLUMNS 2
#define TERMINAL__CURSOR_COLUMNS 1
#define TERMINAL__INPUT_TIMEOUT_MS 50
#define TERMINAL__HIDDEN_DELAY_MS 20
#define TERMINAL__CHILD_STOP_TIMEOUT_MS 500
#define TERMINAL__ASCII_DELETE 0x7f

typedef struct {
    char *transcript;
    uint8_t *transcript_colors;
    size_t transcript_size;
    char input[TERMINAL__LINE_CAPACITY];
    shell_line_editor_t editor;
    char history_draft[TERMINAL__LINE_CAPACITY];
    shell_history_browser_t history;
    terminal_ansi_parser_t ansi;
    bruce_stdio_session_t session;
    bruce_process_id_t child;
    bool dirty;
    bool exit_requested;
} terminal__state_t;

typedef struct {
    size_t start;
    size_t length;
} terminal__visual_line_t;

static void terminal__append(terminal__state_t *state, const char *text, size_t size) {
    terminal_ansi__consume(
        &state->ansi, text, size, state->transcript, state->transcript_colors,
        &state->transcript_size, TERMINAL__TRANSCRIPT_CAPACITY
    );
    state->dirty = true;
}

static void terminal__append_text(terminal__state_t *state, const char *text) {
    terminal__append(state, text, strlen(text));
}

static int terminal__collect_lines(
    const terminal__state_t *state, int columns, int rows, terminal__visual_line_t *lines
) {
    int count = 0;
    size_t start = 0;
    while (start < state->transcript_size) {
        size_t length = 0;
        while (start + length < state->transcript_size && state->transcript[start + length] != '\n' &&
               length < (size_t)columns) {
            length++;
        }
        if (count == rows) {
            memmove(lines, lines + 1, sizeof(*lines) * (size_t)(rows - 1));
            count--;
        }
        lines[count++] = (terminal__visual_line_t){.start = start, .length = length};
        start += length;
        if (start < state->transcript_size && state->transcript[start] == '\n') start++;
        if (length == 0 && start == state->transcript_size) break;
    }
    return count;
}

static void terminal__draw_transcript(
    const terminal__state_t *state, const terminal__visual_line_t *lines, int line_count, uint16_t foreground
) {
    static const uint16_t ansi_colors[TERMINAL_ANSI_COLOR_COUNT] = {
        BRUCE_COLOR_BLACK, BRUCE_COLOR_MAROON, BRUCE_COLOR_DARKGREEN, BRUCE_COLOR_OLIVE,
        BRUCE_COLOR_NAVY, BRUCE_COLOR_PURPLE, BRUCE_COLOR_DARKCYAN, BRUCE_COLOR_LIGHTGREY,
        BRUCE_COLOR_DARKGREY, BRUCE_COLOR_RED, BRUCE_COLOR_GREEN, BRUCE_COLOR_YELLOW,
        BRUCE_COLOR_BLUE, BRUCE_COLOR_MAGENTA, BRUCE_COLOR_CYAN, BRUCE_COLOR_WHITE,
    };
    char text[TERMINAL__LINE_CAPACITY];
    for (int row = 0; row < line_count; ++row) {
        size_t consumed = 0;
        while (consumed < lines[row].length) {
            uint8_t color = state->transcript_colors[lines[row].start + consumed];
            size_t run = 1;
            while (consumed + run < lines[row].length &&
                   state->transcript_colors[lines[row].start + consumed + run] == color) run++;
            memcpy(text, state->transcript + lines[row].start + consumed, run);
            text[run] = '\0';
            display__set_text_color(color < TERMINAL_ANSI_COLOR_COUNT ? ansi_colors[color] : foreground);
            display__set_cursor(
                TERMINAL__FRAME_MARGIN + (int16_t)(consumed * TERMINAL__CHAR_W),
                TERMINAL__TITLE_H + row * TERMINAL__CHAR_H
            );
            display__print(text);
            consumed += run;
        }
    }
}

static void terminal__draw_prompt(
    const terminal__state_t *state, int width, int height, int columns, uint16_t foreground
) {
    int prompt_y = height - TERMINAL__CHAR_H - TERMINAL__FRAME_MARGIN;
    display__draw_line(0, prompt_y - TERMINAL__FRAME_MARGIN, width, prompt_y - TERMINAL__FRAME_MARGIN, foreground);
    display__set_cursor(TERMINAL__FRAME_MARGIN, prompt_y);
    display__set_text_color(foreground);
    display__print(state->child != BRUCE_PROCESS_ID_INVALID ? "> " : "$ ");

    int reserved = TERMINAL__PROMPT_COLUMNS + TERMINAL__CURSOR_COLUMNS;
    size_t visible = (size_t)(columns > reserved ? columns - reserved : 1);
    size_t start = state->editor.cursor > visible ? state->editor.cursor - visible : 0;
    if (start + visible > state->editor.length && state->editor.length > visible) {
        start = state->editor.length - visible;
    }
    size_t shown = state->editor.length - start;
    if (shown > visible) shown = visible;
    char text[TERMINAL__LINE_CAPACITY];
    memcpy(text, state->input + start, shown);
    text[shown] = '\0';
    display__print(text);

    int cursor_x = TERMINAL__FRAME_MARGIN + TERMINAL__PROMPT_COLUMNS * TERMINAL__CHAR_W +
                   (int)(state->editor.cursor - start) * TERMINAL__CHAR_W;
    display__fill_rect(cursor_x, prompt_y + TERMINAL__CHAR_H - 1, TERMINAL__CHAR_W - 1, 1, foreground);
}

static bruce_result_t terminal__draw(const terminal__state_t *state) {
    uint16_t foreground = config__get_pri_color();
    uint16_t background = config__get_bg_color();
    int width = display__width();
    int height = display__height();
    int columns = (width - 2 * TERMINAL__FRAME_MARGIN) / TERMINAL__CHAR_W;
    int rows = (height - TERMINAL__TITLE_H - TERMINAL__CHAR_H - 2 * TERMINAL__FRAME_MARGIN) /
               TERMINAL__CHAR_H;
    if (columns < 1) columns = 1;
    if (rows < 1) rows = 1;
    if (rows > TERMINAL__MAX_VISIBLE_ROWS) rows = TERMINAL__MAX_VISIBLE_ROWS;

    terminal__visual_line_t lines[TERMINAL__MAX_VISIBLE_ROWS];
    int line_count = terminal__collect_lines(state, columns, rows, lines);
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    display__fill_screen(background);
    display__fill_rect(0, 0, width, TERMINAL__TITLE_H, foreground);
    display__set_text_size(1);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_text_color(background);
    display__set_cursor(TERMINAL__FRAME_MARGIN, TERMINAL__FRAME_MARGIN);
    display__print(state->child != BRUCE_PROCESS_ID_INVALID ? "Terminal [running]" : "Terminal");
    terminal__draw_transcript(state, lines, line_count, foreground);
    terminal__draw_prompt(state, width, height, columns, foreground);
    return display__present();
}

static void terminal__drain_output(terminal__state_t *state) {
    char output[256];
    size_t size = 0;
    while (stdio__session_read_output(state->session, output, sizeof(output), &size) == BRUCE_OK) {
        terminal__append(state, output, size);
    }
}

static void terminal__submit(terminal__state_t *state) {
    if (state->editor.length == 0) return;
    if (strcmp(state->input, "clear") == 0) {
        state->transcript_size = 0;
        state->transcript[0] = '\0';
    } else if (strcmp(state->input, "exit") == 0) {
        state->exit_requested = true;
    } else {
        terminal__append(state, state->input, state->editor.length);
        terminal__append(state, "\n", 1);
        (void)stdio__session_write_input(state->session, state->input, state->editor.length);
        (void)stdio__session_write_input(state->session, "\n", 1);
    }
    shell_line_editor__set(&state->editor, "");
    shell_history_browser__reset(&state->history);
    state->dirty = true;
}

static void terminal__open_text_input(terminal__state_t *state) {
    char entered[TERMINAL__LINE_CAPACITY];
    if (dialog__text_input("Terminal", "command", state->input, false, entered, sizeof(entered)) == BRUCE_OK) {
        shell_line_editor__set(&state->editor, entered);
        terminal__submit(state);
    }
    state->dirty = true;
}

static bool terminal__handle_navigation(terminal__state_t *state, int32_t code) {
    switch (code) {
        case BRUCE_INPUT_CODE_DELETE: (void)shell_line_editor__delete(&state->editor); break;
        case BRUCE_INPUT_CODE_LEFT: (void)shell_line_editor__left(&state->editor); break;
        case BRUCE_INPUT_CODE_RIGHT: (void)shell_line_editor__right(&state->editor); break;
        case BRUCE_INPUT_CODE_UP: (void)shell_history_browser__previous(&state->history, &state->editor); break;
        case BRUCE_INPUT_CODE_DOWN: (void)shell_history_browser__next(&state->history, &state->editor); break;
        default: return false;
    }
    state->dirty = true;
    return true;
}

static void terminal__handle_input(terminal__state_t *state, const bruce_input_event_t *event) {
    bool semantic_key = event->type == BRUCE_INPUT_KEY && event->value == 0;
    if ((semantic_key && event->code == BRUCE_INPUT_CODE_BACK) || event->code == BRUCE_INPUT_CODE_BUTTON_B) {
        state->exit_requested = true;
    } else if (event->type == BRUCE_INPUT_KEY && (event->code == '\n' || event->code == '\r')) {
        terminal__submit(state);
    } else if (event->type == BRUCE_INPUT_KEY &&
               (event->code == '\b' || event->code == TERMINAL__ASCII_DELETE)) {
        (void)shell_line_editor__backspace(&state->editor);
        state->dirty = true;
    } else if (semantic_key && terminal__handle_navigation(state, event->code)) {
        return;
    } else if (event->type == BRUCE_INPUT_KEY && event->code >= ' ' && event->code <= '~' && !semantic_key) {
        shell_history_browser__reset(&state->history);
        (void)shell_line_editor__insert(&state->editor, (char)event->code);
        state->dirty = true;
    } else if (event->code == BRUCE_INPUT_CODE_SELECT || event->code == BRUCE_INPUT_CODE_BUTTON_A) {
        terminal__open_text_input(state);
    }
}

/* Rebuilds a shell-style line from startup argv so the terminal submission
 * path can tokenize it again without changing argument boundaries. */
static bool terminal__args_to_line(ArgParser *parser, char *out, size_t out_size) {
    size_t used = 0;
    int argc = ap_count_args(parser);
    for (int i = 0; i < argc; ++i) {
        const char *arg = ap_get_arg_at_index(parser, i);
        size_t needed = i > 0 ? 1u : 0u;
        needed += 2;
        for (const char *p = arg; *p != '\0'; ++p) needed += (*p == '\\' || *p == '"') ? 2u : 1u;
        if (needed >= out_size - used) return false;
        if (i > 0) out[used++] = ' ';
        out[used++] = '"';
        for (const char *p = arg; *p != '\0'; ++p) {
            if (*p == '\\' || *p == '"') out[used++] = '\\';
            out[used++] = *p;
        }
        out[used++] = '"';
    }
    out[used] = '\0';
    return true;
}

int terminal_app_main(int argc, char **argv) {
    char startup_line[TERMINAL__LINE_CAPACITY] = {0};
    bool has_startup_command = false;
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Open the terminal and optionally run a startup command.");
    ap_add_flag(parser, "gui");
    ap_add_flag(parser, "bg");
    ap_add_optional_arg(parser, "command", "Command to run on startup");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP) return 0;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    const char *command = ap_get_arg(parser, "command");
    has_startup_command = command != NULL;
    if (has_startup_command && !terminal__args_to_line(parser, startup_line, sizeof(startup_line))) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(parser);

    if (!app_runner__args_have_background(argc, argv)) {
        bruce_result_t foreground = process__to_foreground();
        if (foreground != BRUCE_OK) return foreground;
    }

    terminal__state_t state = {0};
    state.child = BRUCE_PROCESS_ID_INVALID;
    shell_line_editor__init(&state.editor, state.input, sizeof(state.input));
    shell_history_browser__init(&state.history, state.history_draft, sizeof(state.history_draft));
    terminal_ansi__init(&state.ansi);
    state.transcript = memory__calloc(TERMINAL__TRANSCRIPT_CAPACITY, 1);
    state.transcript_colors = memory__calloc(TERMINAL__TRANSCRIPT_CAPACITY, 1);
    if (state.transcript == NULL || state.transcript_colors == NULL) {
        memory__free(state.transcript);
        memory__free(state.transcript_colors);
        return BRUCE_ERR_NO_MEMORY;
    }
    if (stdio__session_create(&state.session) != BRUCE_OK) {
        memory__free(state.transcript);
        memory__free(state.transcript_colors);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    terminal__append_text(&state, "Bruce terminal\nType a command and press Enter.\n");
    (void)input__flush();

    (void)stdio__session_route_children(state.session);
    int shell_process = app_runner__run("shell", "-i --no-echo", true);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (shell_process <= 0) {
        (void)stdio__session_close(state.session);
        memory__free(state.transcript);
        memory__free(state.transcript_colors);
        return shell_process;
    }
    state.child = (bruce_process_id_t)shell_process;

    if (has_startup_command) {
        shell_line_editor__set(&state.editor, startup_line);
        terminal__submit(&state);
    }

    while (!state.exit_requested) {
        terminal__drain_output(&state);
        if (state.child != BRUCE_PROCESS_ID_INVALID) {
            bruce_process_snapshot_t child;
            if (process__snapshot(state.child, &child) != BRUCE_OK) {
                bruce_process_status_t status;
                (void)process__wait_status(state.child, 0, &status);
                state.child = BRUCE_PROCESS_ID_INVALID;
                state.exit_requested = true;
                state.dirty = true;
            }
        }
        if (state.dirty) {
            bruce_result_t draw = terminal__draw(&state);
            if (draw == BRUCE_OK) state.dirty = false;
        }

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, TERMINAL__INPUT_TIMEOUT_MS);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            (void)runtime__delay(TERMINAL__HIDDEN_DELAY_MS);
            state.dirty = true;
            continue;
        }
        if (input_result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) continue;
        terminal__handle_input(&state, &event);
    }

    if (state.child != BRUCE_PROCESS_ID_INVALID) {
        (void)process__terminate(state.child);
        bruce_process_status_t status;
        if (process__wait_status(state.child, TERMINAL__CHILD_STOP_TIMEOUT_MS, &status) != BRUCE_OK) {
            (void)process__kill(state.child);
            (void)process__wait_status(state.child, TERMINAL__CHILD_STOP_TIMEOUT_MS, &status);
        }
    }
    (void)stdio__session_close(state.session);
    memory__free(state.transcript);
    memory__free(state.transcript_colors);
    return 0;
}
