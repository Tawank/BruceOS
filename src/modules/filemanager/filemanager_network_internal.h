#pragma once

/* Pure parsing helpers factored out of filemanager_network.c's provider
 * registry so the selftest module can unit-test them directly
 * (selftest__run_filemanager_network_* cases in
 * modules/selftest/filemanager_network_test.c) without touching storage or
 * spawning a provider process. Not part of the public core_sdk/ API: other
 * modules must not include this header, only filemanager_app.h.
 */

#include <stdbool.h>
#include <stddef.h>

#define FILEMANAGER_NETWORK_PROVIDER_NAME_MAX 32
#define FILEMANAGER_NETWORK_DISCOVERY_MAX 96

typedef struct {
    char name[FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];    /* Prefix shown/stored under "/Network", e.g. "sftp". */
    char program[FILEMANAGER_NETWORK_PROVIDER_NAME_MAX]; /* App run to open a discovered location. */
    char discovery[FILEMANAGER_NETWORK_DISCOVERY_MAX];   /* Full command line run to list locations. */
} filemanager_network__provider_t;

/* Parses "/config/filemanager.conf"'s JSON -- a bare array of
 * {"name", "program", "discovery"} objects, e.g.
 *   [{"name": "sftp", "program": "sftp", "discovery": "sftp list --autodiscover"}]
 * (a top-level {"providers": [...]} object is accepted too) -- into
 * `providers`, stopping at `max_providers`. "discovery" is optional and
 * defaults to "<program> list --autodiscover", the invocation every
 * provider used before this became configurable; "name" and "program" are
 * required and a provider missing either, or whose "name" contains a space
 * (it has to survive being split back out of "<name> <label>" under
 * "/Network" -- see filemanager_network__split_entry_name()), is skipped.
 * Returns false (leaving *out_count at 0) only when `json_text` itself isn't
 * parseable JSON shaped like a provider list at all. */
bool filemanager_network__parse_providers_json(
    const char *json_text, filemanager_network__provider_t *providers, size_t max_providers, size_t *out_count
);

/* Replaces '/' and '\\' with '_' so a discovered display name is always a
 * safe single path component under "/Network". */
void filemanager_network__sanitize_name(const char *name, char *out, size_t out_size);

/* Splits a "/Network" entry's file name "<provider name> <label>" at its
 * first space, copying the provider name into `name_out`. A label may
 * itself contain spaces (only the first one is the boundary); returns false
 * if `entry_name` has no space at all (or starts with one), i.e. isn't
 * shaped like a provider-owned entry. */
bool filemanager_network__split_entry_name(const char *entry_name, char *name_out, size_t name_out_size);
