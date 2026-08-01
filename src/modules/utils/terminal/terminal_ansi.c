#include "terminal_ansi.h"

#include <stdbool.h>
#include <string.h>

#define TERMINAL_ANSI_ESCAPE_BYTE 0x1b
#define TERMINAL_ANSI_CSI_FINAL_MIN 0x40
#define TERMINAL_ANSI_CSI_FINAL_MAX 0x7e

#define TERMINAL_ANSI_SGR_RESET 0
#define TERMINAL_ANSI_SGR_BOLD 1
#define TERMINAL_ANSI_SGR_NORMAL 22
#define TERMINAL_ANSI_SGR_DEFAULT_FG 39
#define TERMINAL_ANSI_SGR_DARK_FG_MIN 30
#define TERMINAL_ANSI_SGR_DARK_FG_MAX 37
#define TERMINAL_ANSI_SGR_BRIGHT_FG_MIN 90
#define TERMINAL_ANSI_SGR_BRIGHT_FG_MAX 97
#define TERMINAL_ANSI_BRIGHT_OFFSET 8
#define TERMINAL_ANSI_ERASE_ALL 2

static void terminal_ansi__apply_sgr(terminal_ansi_parser_t *parser, unsigned value) {
    if (value == TERMINAL_ANSI_SGR_RESET) {
        parser->color = TERMINAL_ANSI_DEFAULT_COLOR;
        parser->bright = false;
    } else if (value == TERMINAL_ANSI_SGR_BOLD) {
        parser->bright = true;
        if (parser->color < TERMINAL_ANSI_BRIGHT_OFFSET) parser->color += TERMINAL_ANSI_BRIGHT_OFFSET;
    } else if (value == TERMINAL_ANSI_SGR_NORMAL) {
        parser->bright = false;
        if (parser->color >= TERMINAL_ANSI_BRIGHT_OFFSET && parser->color < TERMINAL_ANSI_COLOR_COUNT) {
            parser->color -= TERMINAL_ANSI_BRIGHT_OFFSET;
        }
    } else if (value == TERMINAL_ANSI_SGR_DEFAULT_FG) {
        parser->color = TERMINAL_ANSI_DEFAULT_COLOR;
    } else if (value >= TERMINAL_ANSI_SGR_DARK_FG_MIN && value <= TERMINAL_ANSI_SGR_DARK_FG_MAX) {
        parser->color = (uint8_t)(value - TERMINAL_ANSI_SGR_DARK_FG_MIN +
                                  (parser->bright ? TERMINAL_ANSI_BRIGHT_OFFSET : 0));
    } else if (value >= TERMINAL_ANSI_SGR_BRIGHT_FG_MIN && value <= TERMINAL_ANSI_SGR_BRIGHT_FG_MAX) {
        parser->color = (uint8_t)(value - TERMINAL_ANSI_SGR_BRIGHT_FG_MIN + TERMINAL_ANSI_BRIGHT_OFFSET);
    }
}

static void terminal_ansi__append_char(
    terminal_ansi_parser_t *parser, char c, char *transcript, uint8_t *colors,
    size_t *size, size_t capacity
) {
    if (c == '\b') {
        if (parser->cursor > 0 && transcript[parser->cursor - 1] != '\n') parser->cursor--;
        return;
    }
    if (c == '\r') {
        while (parser->cursor > 0 && transcript[parser->cursor - 1] != '\n') parser->cursor--;
        return;
    }
    if ((unsigned char)c < ' ' && c != '\n' && c != '\t') return;
    if (c == '\t') c = ' ';
    if (c == '\n' && parser->cursor < *size) {
        while (parser->cursor < *size && transcript[parser->cursor] != '\n') parser->cursor++;
        if (parser->cursor < *size) {
            parser->cursor++;
            return;
        }
    }
    if (*size == capacity - 1) {
        memmove(transcript, transcript + 1, *size - 1);
        memmove(colors, colors + 1, *size - 1);
        (*size)--;
        if (parser->cursor > 0) parser->cursor--;
    }
    if (parser->cursor < *size && transcript[parser->cursor] == '\n') {
        memmove(transcript + parser->cursor + 1, transcript + parser->cursor, *size - parser->cursor);
        memmove(colors + parser->cursor + 1, colors + parser->cursor, *size - parser->cursor);
        (*size)++;
    }
    if (parser->cursor < *size) {
        transcript[parser->cursor] = c;
        colors[parser->cursor++] = parser->color;
        return;
    }
    transcript[*size] = c;
    colors[(*size)++] = parser->color;
    parser->cursor = *size;
}

static void terminal_ansi__push_param(terminal_ansi_parser_t *parser) {
    if (parser->param_count < sizeof(parser->params) / sizeof(parser->params[0])) {
        parser->params[parser->param_count++] = parser->has_value ? parser->value : 0;
    }
    parser->value = 0;
    parser->has_value = false;
}

static void terminal_ansi__finish_csi(
    terminal_ansi_parser_t *parser, unsigned char final, char *transcript, size_t *size
) {
    if (final == 'm') {
        terminal_ansi__push_param(parser);
        for (uint8_t i = 0; i < parser->param_count; ++i) terminal_ansi__apply_sgr(parser, parser->params[i]);
    } else if (final == 'J' && parser->value == TERMINAL_ANSI_ERASE_ALL) {
        *size = 0;
        parser->cursor = 0;
    } else if (final == 'K' && parser->value == TERMINAL_ANSI_ERASE_ALL) {
        while (*size > 0 && transcript[*size - 1] != '\n') (*size)--;
        if (parser->cursor > *size) parser->cursor = *size;
    } else if (final == 'D') {
        size_t amount = parser->has_value && parser->value > 0 ? parser->value : 1;
        while (amount-- > 0 && parser->cursor > 0 && transcript[parser->cursor - 1] != '\n') {
            parser->cursor--;
        }
    } else if (final == 'C') {
        size_t amount = parser->has_value && parser->value > 0 ? parser->value : 1;
        while (amount-- > 0 && parser->cursor < *size && transcript[parser->cursor] != '\n') {
            parser->cursor++;
        }
    } else if (final == 'G') {
        size_t column = parser->has_value && parser->value > 0 ? parser->value - 1u : 0;
        while (parser->cursor > 0 && transcript[parser->cursor - 1] != '\n') parser->cursor--;
        while (column-- > 0 && parser->cursor < *size && transcript[parser->cursor] != '\n') {
            parser->cursor++;
        }
    }
    parser->state = TERMINAL_ANSI_TEXT;
}

static void terminal_ansi__consume_byte(
    terminal_ansi_parser_t *parser, unsigned char byte, char *transcript, uint8_t *colors,
    size_t *size, size_t capacity
) {
    switch (parser->state) {
        case TERMINAL_ANSI_TEXT:
            if (byte == TERMINAL_ANSI_ESCAPE_BYTE) parser->state = TERMINAL_ANSI_ESCAPE;
            else terminal_ansi__append_char(parser, (char)byte, transcript, colors, size, capacity);
            break;
        case TERMINAL_ANSI_ESCAPE:
            if (byte == '[') {
                parser->state = TERMINAL_ANSI_CSI;
                parser->value = 0;
                parser->has_value = false;
                parser->param_count = 0;
            } else if (byte == ']') {
                parser->state = TERMINAL_ANSI_OSC;
            } else {
                parser->state = TERMINAL_ANSI_TEXT;
            }
            break;
        case TERMINAL_ANSI_OSC:
            if (byte == '\a') parser->state = TERMINAL_ANSI_TEXT;
            else if (byte == TERMINAL_ANSI_ESCAPE_BYTE) parser->state = TERMINAL_ANSI_ESCAPE;
            break;
        case TERMINAL_ANSI_CSI:
            if (byte >= '0' && byte <= '9') {
                parser->value = (uint16_t)(parser->value * 10u + (uint16_t)(byte - '0'));
                parser->has_value = true;
            } else if (byte == ';') {
                terminal_ansi__push_param(parser);
            } else if (byte >= TERMINAL_ANSI_CSI_FINAL_MIN && byte <= TERMINAL_ANSI_CSI_FINAL_MAX) {
                terminal_ansi__finish_csi(parser, byte, transcript, size);
            }
            break;
    }
}

void terminal_ansi__init(terminal_ansi_parser_t *parser) {
    memset(parser, 0, sizeof(*parser));
    parser->color = TERMINAL_ANSI_DEFAULT_COLOR;
}

void terminal_ansi__consume(
    terminal_ansi_parser_t *parser, const char *input, size_t input_size,
    char *transcript, uint8_t *colors, size_t *transcript_size, size_t capacity
) {
    for (size_t i = 0; i < input_size; ++i) {
        terminal_ansi__consume_byte(
            parser, (unsigned char)input[i], transcript, colors, transcript_size, capacity
        );
    }
    transcript[*transcript_size] = '\0';
}
