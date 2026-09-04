#pragma once

/* Pure parsing helpers factored out of filemanager_app.c's "Network folder"
 * section so the selftest module can unit-test them directly
 * (selftest__run_filemanager_network_* cases in
 * modules/selftest/filemanager_network_test.c) without touching storage or
 * spawning a provider process. Not part of the public core_sdk/ API: other
 * modules must not include this header, only filemanager_app.h.
 */

#include <stddef.h>

#define FILEMANAGER_NETWORK_PROVIDER_NAME_MAX 32

/* Parses provider names out of `text` (one per line, "#"-comments, leading/
 * trailing whitespace and "\r" trimmed, blank/oversized/comment lines
 * skipped) into `providers`, stopping at `max_providers`. Mutates `text` in
 * place (strtok_r), same as the file-backed caller that owns the buffer. */
void filemanager__network_parse_providers(
    char *text, char providers[][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX], size_t max_providers, size_t *out_count
);

/* Replaces '/' and '\\' with '_' so a discovered display name is always a
 * safe single path component under "/Network". */
void filemanager__network_sanitize_name(const char *name, char *out, size_t out_size);
