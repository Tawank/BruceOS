#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Just enough JSON reading for the search and routing replies. */
typedef struct {
    const char *p;
    const char *end;
} json_t;

void json__skip_space(json_t *json);
/* Consumes `c` after any spaces; false when something else is next. */
bool json__eat(json_t *json, char c);
/* Reads a string into `out` (truncated on a character boundary), or skips it when `out` is NULL. */
bool json__string(json_t *json, char *out, size_t capacity);
bool json__skip_value(json_t *json);
/* A number, or a string holding one. */
bool json__number(json_t *json, double *out);
