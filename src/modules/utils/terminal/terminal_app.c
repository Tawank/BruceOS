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
#include "terminal_ansi.h"

#define TERMINAL__TRANSCRIPT_CAPACITY 4096
#define TERMINAL__LINE_CAPACITY 512
/* Mirrors the active font's cell size (display__get_font_metrics()) as
 * compile-time constants -- every built-in font is monospace 6x10 today. */
#define TERMINAL__CHAR_W 6
#define TERMINAL__CHAR_H 10
#define TERMINAL__TITLE_H 12
#define TERMINAL__MAX_VISIBLE_ROWS 40
#define TERMINAL__FRAME_MARGIN 2
#define TERMINAL__INPUT_TIMEOUT_MS 50
#define TERMINAL__HIDDEN_DELAY_MS 20
#define TERMINAL__CHILD_STOP_TIMEOUT_MS 500
#define TERMINAL__ASCII_ESCAPE 0x1b
#define TERMINAL__ASCII_DELETE 0x7f

typedef struct {
    char *transcript;
    uint8_t *transcript_colors;
    size_t transcript_size;
    terminal_ansi_parser_t ansi;
    bruce_stdio_session_t session;
    bruce_process_id_t child;
    bruce_memory_object_t transcript_object;
    bruce_memory_object_t colors_object;
    bool transcript_external;
    bool colors_external;
    bool dirty;
    bool exit_requested;
} terminal__state_t;

/* Prefers a PSRAM- or plain-internal-RAM-backed memory__external block (both
 * are plain, directly addressable buffers) so the parser can keep mutating it
 * in place with ordinary pointer writes. Falls back to a separate, untracked
 * internal heap allocation when PSRAM has no room for `size`, without ever
 * calling memory__external_alloc() in that case: on a board where a "swap"
 * partition has been committed (see partition_manager__commit()),
 * memory__external_alloc() would otherwise land on it whenever PSRAM is
 * short, which means erasing a real 64 KiB flash region just to get a
 * SWAP-backed object this function immediately frees because it's not
 * safely writable through a raw pointer -- only through
 * memory__external_write(). That erase is real hardware time (a 64 KiB
 * region is 16 flash sectors) wasted on a result this never uses, and was
 * the whole cause of a plain terminal open going from instant to ~700ms
 * once a swap partition existed to find. memory__get_stats() only reads
 * already-tracked heap/page-bitmap counters, so checking it first is cheap
 * regardless of which way it comes out. */
static bruce_result_t
terminal__alloc_buffer(void **out_data, bruce_memory_object_t *out_object, bool *out_external, size_t size) {
    bruce_memory_stats_t stats;
    bool psram_has_room =
        memory__get_stats(&stats) == BRUCE_OK && stats.psram_largest_block >= size;
    bruce_memory_object_t object;
    if (psram_has_room && memory__external_alloc(size, &object) == BRUCE_OK) {
        const void *mapped = NULL;
        if ((object.backend == BRUCE_MEMORY_BACKEND_PSRAM ||
             object.backend == BRUCE_MEMORY_BACKEND_INTERNAL) &&
            memory__external_map(&object, &mapped) == BRUCE_OK) {
            memset((void *)mapped, 0, size);
            *out_data = (void *)mapped;
            *out_object = object;
            *out_external = true;
            return BRUCE_OK;
        }
        (void)memory__external_free(&object);
    }
    *out_data = memory__calloc(size, 1);
    *out_external = false;
    return *out_data != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
}

static void terminal__free_buffer(void *data, bruce_memory_object_t *object, bool external) {
    if (external) {
        (void)memory__external_free(object);
    } else {
        memory__free(data);
    }
}

static void terminal__free_buffers(terminal__state_t *state) {
    terminal__free_buffer(state->transcript, &state->transcript_object, state->transcript_external);
    terminal__free_buffer(state->transcript_colors, &state->colors_object, state->colors_external);
}

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
    bool cursor_needs_row = state->transcript_size == 0 ||
                            state->transcript[state->transcript_size - 1] == '\n' ||
                            (count > 0 && lines[count - 1].length == (size_t)columns);
    if (cursor_needs_row) {
        if (count == rows) {
            memmove(lines, lines + 1, sizeof(*lines) * (size_t)(rows - 1));
            count--;
        }
        lines[count++] = (terminal__visual_line_t){.start = state->transcript_size, .length = 0};
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

static void terminal__draw_cursor(
    const terminal__state_t *state, const terminal__visual_line_t *lines, int line_count,
    uint16_t foreground
) {
    if (line_count == 0) return;
    int cursor_row = line_count - 1;
    size_t cursor_column = lines[cursor_row].length;
    for (int row = line_count - 1; row >= 0; --row) {
        size_t end = lines[row].start + lines[row].length;
        if (state->ansi.cursor >= lines[row].start && state->ansi.cursor <= end) {
            cursor_row = row;
            cursor_column = state->ansi.cursor - lines[row].start;
            break;
        }
    }
    int cursor_x = TERMINAL__FRAME_MARGIN + (int)cursor_column * TERMINAL__CHAR_W;
    int cursor_y = TERMINAL__TITLE_H + cursor_row * TERMINAL__CHAR_H;
    display__fill_rect(cursor_x, cursor_y + TERMINAL__CHAR_H - 1, TERMINAL__CHAR_W - 1, 1, foreground);
}

static bruce_result_t terminal__draw(const terminal__state_t *state) {
    uint16_t foreground = config__get_theme_primary();
    uint16_t background = config__get_theme_background();
    int width = display__width();
    int height = display__height();
    int columns = (width - 2 * TERMINAL__FRAME_MARGIN) / TERMINAL__CHAR_W;
    int rows = (height - TERMINAL__TITLE_H - 2 * TERMINAL__FRAME_MARGIN) / TERMINAL__CHAR_H;
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
    terminal__draw_cursor(state, lines, line_count, foreground);
    return display__present();
}

static void terminal__drain_output(terminal__state_t *state) {
    char output[256];
    size_t size = 0;
    while (stdio__session_read_output(state->session, output, sizeof(output), &size) == BRUCE_OK) {
        terminal__append(state, output, size);
    }
}

static void terminal__write_input(terminal__state_t *state, const char *bytes, size_t size) {
    (void)stdio__session_write_input(state->session, bytes, size);
}

static void terminal__open_text_input(terminal__state_t *state) {
    char entered[TERMINAL__LINE_CAPACITY];
    if (dialog__text_input("Terminal", "input", NULL, false, entered, sizeof(entered)) == BRUCE_OK) {
        terminal__write_input(state, entered, strlen(entered));
        terminal__write_input(state, "\r", 1);
    }
    state->dirty = true;
}

static void terminal__handle_input(terminal__state_t *state, const bruce_input_event_t *event) {
    bool semantic_key = event->type == BRUCE_INPUT_KEY && event->value != event->code;
    if ((semantic_key && event->code == BRUCE_INPUT_CODE_BACK) ||
        (event->type == BRUCE_INPUT_KEY && event->code == TERMINAL__ASCII_ESCAPE) ||
        event->code == BRUCE_INPUT_CODE_BUTTON_B) {
        state->exit_requested = true;
        return;
    }
    if ((event->code == BRUCE_INPUT_CODE_SELECT && (semantic_key || event->type != BRUCE_INPUT_KEY)) ||
        event->code == BRUCE_INPUT_CODE_BUTTON_A) {
        terminal__open_text_input(state);
        return;
    }
    if (semantic_key || event->type != BRUCE_INPUT_KEY) {
        const char *sequence = NULL;
        switch (event->code) {
            case BRUCE_INPUT_CODE_UP: sequence = "\033[A"; break;
            case BRUCE_INPUT_CODE_DOWN: sequence = "\033[B"; break;
            case BRUCE_INPUT_CODE_RIGHT: sequence = "\033[C"; break;
            case BRUCE_INPUT_CODE_LEFT: sequence = "\033[D"; break;
            case BRUCE_INPUT_CODE_HOME: sequence = "\033[H"; break;
            case BRUCE_INPUT_CODE_DELETE: sequence = "\033[3~"; break;
            default: break;
        }
        if (sequence != NULL) terminal__write_input(state, sequence, strlen(sequence));
        return;
    }
    if (event->code == '\n' || event->code == '\r') {
        terminal__write_input(state, "\r", 1);
    } else if (event->code == '\b' || event->code == TERMINAL__ASCII_DELETE) {
        terminal__write_input(state, "\177", 1);
    } else if (event->code > 0 && event->code <= TERMINAL__ASCII_DELETE) {
        char byte = (char)event->code;
        terminal__write_input(state, &byte, 1);
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

    terminal__state_t state = {0};
    state.child = BRUCE_PROCESS_ID_INVALID;
    terminal_ansi__init(&state.ansi);
    void *transcript_data = NULL;
    void *colors_data = NULL;
    bruce_result_t transcript_alloc = terminal__alloc_buffer(
        &transcript_data, &state.transcript_object, &state.transcript_external, TERMINAL__TRANSCRIPT_CAPACITY
    );
    state.transcript = transcript_data;
    bruce_result_t colors_alloc = terminal__alloc_buffer(
        &colors_data, &state.colors_object, &state.colors_external, TERMINAL__TRANSCRIPT_CAPACITY
    );
    state.transcript_colors = colors_data;
    if (transcript_alloc != BRUCE_OK || colors_alloc != BRUCE_OK) {
        if (transcript_alloc == BRUCE_OK) {
            terminal__free_buffer(state.transcript, &state.transcript_object, state.transcript_external);
        }
        if (colors_alloc == BRUCE_OK) {
            terminal__free_buffer(state.transcript_colors, &state.colors_object, state.colors_external);
        }
        return BRUCE_ERR_NO_MEMORY;
    }
    if (stdio__session_create(&state.session) != BRUCE_OK) {
        terminal__free_buffers(&state);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    terminal__append_text(&state, "Bruce terminal\nType a command and press Enter.\n");
    (void)input__flush();

    (void)stdio__session_route_children(state.session);
    int shell_process = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (shell_process <= 0) {
        (void)stdio__session_close(state.session);
        terminal__free_buffers(&state);
        return shell_process;
    }
    state.child = (bruce_process_id_t)shell_process;

    if (has_startup_command) {
        terminal__write_input(&state, startup_line, strlen(startup_line));
        terminal__write_input(&state, "\r", 1);
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
    terminal__free_buffers(&state);
    return 0;
}
