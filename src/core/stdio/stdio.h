#pragma once

#include <stdio.h>

#include "core_sdk/stdio.h"

void stdio__task_attach(bruce_stdio_session_t session, FILE **out_input, FILE **out_output, FILE **out_error);
void stdio__task_detach(FILE *input, FILE *output, FILE *error);
bruce_result_t stdio__session_read_input(bruce_stdio_session_t session, void *buffer, size_t capacity,
                                         uint32_t timeout_ms, size_t *out_size);
