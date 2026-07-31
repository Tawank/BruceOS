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

#define TERMINAL__TRANSCRIPT_CAPACITY 4096
#define TERMINAL__LINE_CAPACITY 256
#define TERMINAL__CHAR_W 6
#define TERMINAL__CHAR_H 8
#define TERMINAL__TITLE_H 12
#define TERMINAL__MAX_VISIBLE_ROWS 40

typedef struct {
    char *transcript;
    size_t transcript_size;
    char input[TERMINAL__LINE_CAPACITY];
    size_t input_size;
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
    for (size_t i = 0; i < size; ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (state->transcript_size == TERMINAL__TRANSCRIPT_CAPACITY - 1) {
            memmove(state->transcript, state->transcript + 1, state->transcript_size - 1);
            state->transcript_size--;
        }
        state->transcript[state->transcript_size++] = c;
    }
    state->transcript[state->transcript_size] = '\0';
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

static bruce_result_t terminal__draw(const terminal__state_t *state) {
    uint16_t foreground = config__get_pri_color();
    uint16_t background = config__get_bg_color();
    int width = display__width();
    int height = display__height();
    int columns = (width - 4) / TERMINAL__CHAR_W;
    int rows = (height - TERMINAL__TITLE_H - TERMINAL__CHAR_H - 4) / TERMINAL__CHAR_H;
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
    display__set_cursor(2, 2);
    display__print(state->child != BRUCE_PROCESS_ID_INVALID ? "Terminal [running]" : "Terminal");

    display__set_text_color(foreground);
    char line[TERMINAL__LINE_CAPACITY];
    for (int i = 0; i < line_count; ++i) {
        size_t length = lines[i].length < sizeof(line) - 1 ? lines[i].length : sizeof(line) - 1;
        memcpy(line, state->transcript + lines[i].start, length);
        line[length] = '\0';
        display__set_cursor(2, TERMINAL__TITLE_H + i * TERMINAL__CHAR_H);
        display__print(line);
    }

    int prompt_y = height - TERMINAL__CHAR_H - 2;
    display__draw_line(0, prompt_y - 2, width, prompt_y - 2, foreground);
    display__set_cursor(2, prompt_y);
    display__print(state->child != BRUCE_PROCESS_ID_INVALID ? "> " : "$ ");
    size_t visible = (size_t)(columns > 2 ? columns - 2 : 1);
    const char *input =
        state->input_size > visible ? state->input + state->input_size - visible : state->input;
    display__print(input);
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
    if (state->input_size == 0) return;
    state->input[state->input_size] = '\0';
    if (strcmp(state->input, "clear") == 0) {
        state->transcript_size = 0;
        state->transcript[0] = '\0';
    } else if (strcmp(state->input, "exit") == 0) {
        state->exit_requested = true;
    } else {
        terminal__append(state, state->input, state->input_size);
        (void)stdio__session_write_input(state->session, state->input, state->input_size);
        (void)stdio__session_write_input(state->session, "\n", 1);
    }
    state->input_size = 0;
    state->input[0] = '\0';
    state->dirty = true;
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
    state.transcript = memory__calloc(TERMINAL__TRANSCRIPT_CAPACITY, 1);
    if (state.transcript == NULL) return BRUCE_ERR_NO_MEMORY;
    if (stdio__session_create(&state.session) != BRUCE_OK) {
        memory__free(state.transcript);
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
        return shell_process;
    }
    state.child = (bruce_process_id_t)shell_process;

    if (has_startup_command) {
        memcpy(state.input, startup_line, sizeof(state.input));
        state.input_size = strlen(state.input);
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
        bruce_result_t input_result = input__read(&event, 50);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            (void)runtime__delay(20);
            state.dirty = true;
            continue;
        }
        if (input_result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) continue;
        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B) {
            state.exit_requested = true;
        } else if (event.type == BRUCE_INPUT_KEY && (event.code == '\n' || event.code == '\r')) {
            terminal__submit(&state);
        } else if (event.type == BRUCE_INPUT_KEY && (event.code == '\b' || event.code == 0x7f)) {
            if (state.input_size > 0) state.input[--state.input_size] = '\0';
            state.dirty = true;
        } else if (event.type == BRUCE_INPUT_KEY && event.code >= 32 && event.code <= 126 &&
                   state.input_size + 1 < sizeof(state.input)) {
            state.input[state.input_size++] = (char)event.code;
            state.input[state.input_size] = '\0';
            state.dirty = true;
        } else if (event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A) {
            char entered[TERMINAL__LINE_CAPACITY];
            if (dialog__text_input("Terminal", "command", state.input, false, entered, sizeof(entered)) ==
                BRUCE_OK) {
                snprintf(state.input, sizeof(state.input), "%s", entered);
                state.input_size = strlen(state.input);
                terminal__submit(&state);
            }
            state.dirty = true;
        }
    }

    if (state.child != BRUCE_PROCESS_ID_INVALID) {
        (void)process__terminate(state.child);
        bruce_process_status_t status;
        if (process__wait_status(state.child, 500, &status) != BRUCE_OK) {
            (void)process__kill(state.child);
            (void)process__wait_status(state.child, 500, &status);
        }
    }
    (void)stdio__session_close(state.session);
    memory__free(state.transcript);
    return 0;
}
