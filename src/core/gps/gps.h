#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/gps.h"

#define GPS_PARSER_TALKERS 6

typedef struct {
    bruce_gps_fix_t fix;
    uint8_t in_view[GPS_PARSER_TALKERS]; /* Per constellation, from GSV. */
} gps_parser_t;

/* Applies one NMEA sentence (RMC, GGA or GSV, any talker, with or without the
 * line ending) to `parser`. Returns false for a bad checksum or another
 * sentence type; *out_position is set when the position changed. */
bool gps__parse_sentence(const char *line, gps_parser_t *parser, bool *out_position);

/* Cuts the receiver's power before deep sleep. */
void gps__power_off(void);
