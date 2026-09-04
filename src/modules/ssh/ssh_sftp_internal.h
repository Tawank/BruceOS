#pragma once

/* Pure parsing/matching helpers factored out of ssh_sftp_app.c so the
 * selftest module can unit-test them directly (selftest__run_sftp_* cases in
 * modules/selftest/ssh_sftp_test.c) without a live SSH/SFTP connection. Not
 * part of the public core_sdk/ API: other modules must not include this
 * header, only ssh_sftp_app.h.
 *
 * These are deliberate near-duplicates of ssh_app.c's equivalents (modules
 * can't share static helpers across files -- see the module-boundary
 * comment at the top of ssh_sftp_app.c); exposing them here doesn't change
 * that, it just gives selftest a seam into this file's own copies.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/ssh.h"
#include "core_sdk/storage.h"

#define SFTP_APP_HOST_MAX 64u
#define SFTP_APP_USERNAME_MAX 64u
#define SFTP_APP_PATH_MAX BRUCE_STORAGE_PATH_MAX

typedef struct {
    char hostname[SFTP_APP_HOST_MAX];
    char username[SFTP_APP_USERNAME_MAX];
    char identity[SFTP_APP_PATH_MAX];
    uint16_t port;
    bool hostname_set;
    bool username_set;
    bool identity_set;
    bool port_set;
} sftp_app__config_t;

/* "/.ssh/config" host-pattern matching (Host directive: "*"/"?" wildcards,
 * "!"-negation, space/tab-separated pattern lists). */
bool sftp_app__parse_port(const char *text, uint16_t *out_port);
bool sftp_app__host_pattern_matches(const char *pattern, const char *host);
bool sftp_app__host_list_matches(char *patterns, const char *host);
bool sftp_app__is_literal_host_token(const char *token);
char *sftp_app__trim(char *text);
bool sftp_app__split_directive(char *line, char **out_value);
bool sftp_app__resolve_identity_path(const char *identity, const char **out_path);
void sftp_app__copy_config_value(char *out, size_t capacity, const char *value, bool *was_set);

/* known_hosts TOFU wire format: "<host>|<port> <64 hex chars>\n". */
void sftp_app__hex_encode(const uint8_t *bytes, size_t size, char *out);
int sftp_app__hex_value(char c);
bool sftp_app__hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_size);
bool sftp_app__find_known_fingerprint(
    const char *buffer, size_t size, const char *key, uint8_t out_fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
);
