#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 3GPP TS 27.010 basic option framing, as the A76XX speaks after AT+CMUX=0. */

#define CMUX_FLAG 0xF9
#define CMUX_SABM 0x2F
#define CMUX_UA 0x63
#define CMUX_DM 0x0F
#define CMUX_DISC 0x43
#define CMUX_UIH 0xEF
#define CMUX_PF 0x10
/* The modem's default N1 (AT+CMUX? reports 1500). */
#define CMUX_DATA_MAX 1500
/* Flags, address, control, two length bytes and the FCS. */
#define CMUX_FRAME_MAX (CMUX_DATA_MAX + 7)

typedef struct {
    uint8_t dlci;
    uint8_t control; /* Without the P/F bit. */
    const uint8_t *data;
    size_t length;
} cmux_frame_t;

typedef struct {
    uint8_t state;
    uint8_t header[4];
    uint8_t header_length;
    uint16_t length;
    uint16_t got;
    uint8_t fcs;
    uint8_t data[CMUX_DATA_MAX];
} cmux_parser_t;

/* Writes one frame from this side (the initiator) to out, which holds
 * CMUX_FRAME_MAX bytes; returns its size, 0 when data is too long. */
size_t cmux__frame(uint8_t dlci, uint8_t control, const uint8_t *data, size_t length, uint8_t *out);

void cmux__reset(cmux_parser_t *parser);

/* Takes one received byte; true when it completes a valid frame, which stays
 * valid until the next call. Bad frames are dropped. */
bool cmux__feed(cmux_parser_t *parser, uint8_t byte, cmux_frame_t *out);
