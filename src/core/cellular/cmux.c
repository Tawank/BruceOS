#include "cmux.h"

#include <string.h>

enum { CMUX__FLAG, CMUX__ADDRESS, CMUX__CONTROL, CMUX__LENGTH, CMUX__LENGTH2, CMUX__DATA, CMUX__FCS, CMUX__END };

/* 27.010's CRC-8 (x^8 + x^2 + x + 1, reflected), over address, control and length. */
static uint8_t cmux__crc(const uint8_t *bytes, size_t count) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 1) ? (uint8_t)((crc >> 1) ^ 0xE0) : (uint8_t)(crc >> 1);
    }
    return crc;
}

size_t cmux__frame(uint8_t dlci, uint8_t control, const uint8_t *data, size_t length, uint8_t *out) {
    if (length > CMUX_DATA_MAX) return 0;
    size_t at = 0;
    out[at++] = CMUX_FLAG;
    out[at++] = (uint8_t)((dlci << 2) | 0x03); /* C/R set: commands from the initiator. */
    out[at++] = control;
    if (length < 128) {
        out[at++] = (uint8_t)((length << 1) | 1);
    } else {
        out[at++] = (uint8_t)((length & 0x7F) << 1);
        out[at++] = (uint8_t)(length >> 7);
    }
    uint8_t fcs = (uint8_t)(0xFF - cmux__crc(out + 1, at - 1));
    if (length > 0) memcpy(out + at, data, length);
    at += length;
    out[at++] = fcs;
    out[at++] = CMUX_FLAG;
    return at;
}

void cmux__reset(cmux_parser_t *parser) { parser->state = CMUX__FLAG; }

bool cmux__feed(cmux_parser_t *parser, uint8_t byte, cmux_frame_t *out) {
    switch (parser->state) {
        case CMUX__FLAG:
            if (byte == CMUX_FLAG) parser->state = CMUX__ADDRESS;
            return false;
        case CMUX__ADDRESS:
            if (byte == CMUX_FLAG) return false;
            parser->header[0] = byte;
            parser->header_length = 1;
            parser->state = CMUX__CONTROL;
            return false;
        case CMUX__CONTROL:
            parser->header[parser->header_length++] = byte;
            parser->state = CMUX__LENGTH;
            return false;
        case CMUX__LENGTH:
            parser->header[parser->header_length++] = byte;
            parser->length = byte >> 1;
            parser->got = 0;
            if (!(byte & 1)) {
                parser->state = CMUX__LENGTH2;
                return false;
            }
            break;
        case CMUX__LENGTH2:
            parser->header[parser->header_length++] = byte;
            parser->length |= (uint16_t)(byte << 7);
            parser->got = 0;
            break;
        case CMUX__DATA:
            parser->data[parser->got++] = byte;
            if (parser->got == parser->length) parser->state = CMUX__FCS;
            return false;
        case CMUX__FCS:
            parser->fcs = byte;
            parser->state = CMUX__END;
            return false;
        default:
            parser->state = byte == CMUX_FLAG ? CMUX__ADDRESS : CMUX__FLAG;
            if (byte != CMUX_FLAG) return false;
            uint8_t check[5];
            memcpy(check, parser->header, parser->header_length);
            check[parser->header_length] = parser->fcs;
            if (cmux__crc(check, parser->header_length + 1u) != 0xCF) return false;
            out->dlci = parser->header[0] >> 2;
            out->control = parser->header[1] & (uint8_t)~CMUX_PF;
            out->data = parser->data;
            out->length = parser->length;
            return true;
    }
    /* After the length. */
    if (parser->length > CMUX_DATA_MAX) {
        parser->state = CMUX__FLAG;
    } else {
        parser->state = parser->length > 0 ? CMUX__DATA : CMUX__FCS;
    }
    return false;
}
