#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERMINAL_ANSI_DEFAULT_COLOR UINT8_MAX
#define TERMINAL_ANSI_COLOR_COUNT 16

typedef enum {
    TERMINAL_ANSI_TEXT,
    TERMINAL_ANSI_ESCAPE,
    TERMINAL_ANSI_CSI,
    TERMINAL_ANSI_OSC,
} terminal_ansi_parse_state_t;

typedef struct {
    terminal_ansi_parse_state_t state;
    size_t cursor;
    uint16_t value;
    uint16_t params[8];
    uint8_t param_count;
    uint8_t color;
    bool bright;
    bool has_value;
} terminal_ansi_parser_t;

void terminal_ansi__init(terminal_ansi_parser_t *parser);
void terminal_ansi__consume(
    terminal_ansi_parser_t *parser, const char *input, size_t input_size,
    char *transcript, uint8_t *colors, size_t *transcript_size, size_t capacity
);
