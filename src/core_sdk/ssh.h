#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/process.h"
#include "core_sdk/result.h"

#define BRUCE_SSH_HOST_KEY_SHA256_SIZE 32
#define BRUCE_SSH_PRIVATE_KEY_MAX_SIZE 512
#define BRUCE_SSH_PUBLIC_KEY_MAX_SIZE 256

typedef enum {
    BRUCE_SSH_KEY_ECDSA_P256 = 0,
    BRUCE_SSH_KEY_ED25519,
} bruce_ssh_key_type_t;

/* SSH sessions are opaque, owned by the calling process, require the `ssh`
 * permission, and close automatically when that process exits. The caller
 * must verify the host-key fingerprint before authenticating. */
bruce_result_t ssh__connect(
    const char *host, uint16_t port, uint32_t timeout_ms, bruce_ssh_id_t *out_session
);
bruce_result_t ssh__host_key_sha256(
    bruce_ssh_id_t session, uint8_t out_fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
);
/* Compares an expected fingerprint to the negotiated host key. Authentication
 * remains unavailable until this succeeds. */
bruce_result_t ssh__verify_host_key_sha256(
    bruce_ssh_id_t session, const uint8_t expected[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
);
bruce_result_t ssh__authenticate_password(
    bruce_ssh_id_t session, const char *username, const char *password, uint32_t timeout_ms
);
/* Generates the selected key type. Ed25519 private keys use the unencrypted
 * OpenSSH format; ECDSA P-256 uses SEC1 PEM. Text sizes exclude the trailing
 * NUL, and the caller owns both buffers. */
bruce_result_t ssh__generate_keypair_ex(
    bruce_ssh_key_type_t type, char *private_key, size_t private_capacity,
    size_t *out_private_size, char *public_key, size_t public_capacity,
    size_t *out_public_size
);
/* Authenticates with an unencrypted ECDSA P-256 SEC1 PEM key or an unencrypted
 * OpenSSH Ed25519 private key. */
bruce_result_t ssh__authenticate_key(
    bruce_ssh_id_t session, const char *username, const void *private_key,
    size_t private_key_size, uint32_t timeout_ms
);
bruce_result_t ssh__open_shell(
    bruce_ssh_id_t session, const char *terminal_type, uint16_t columns, uint16_t rows,
    uint32_t timeout_ms
);
bruce_result_t ssh__resize_pty(
    bruce_ssh_id_t session, uint16_t columns, uint16_t rows, uint32_t timeout_ms
);
/* EOF is BRUCE_OK with *out_size == 0. Set `stderr_stream` to read the SSH
 * channel's extended-data stream. */
bruce_result_t ssh__read(
    bruce_ssh_id_t session, bool stderr_stream, void *buffer, size_t capacity,
    uint32_t timeout_ms, size_t *out_size
);
bruce_result_t ssh__write(
    bruce_ssh_id_t session, const void *buffer, size_t size, uint32_t timeout_ms,
    size_t *out_size
);
bruce_result_t ssh__close(bruce_ssh_id_t session);
