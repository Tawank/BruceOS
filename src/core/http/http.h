#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_client.h"

/* Shared by the blocking (http.c) and background (http_async.c) clients. */
esp_http_client_method_t http__method_from_string(const char *method);
bool http__is_redirect_status(int status_code);
bool http__parse_content_length(const char *value, size_t *out_length);
