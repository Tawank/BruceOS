#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/process.h"
#include "core_sdk/result.h"

/**
 * @brief SSH client: connect, host-key verification, authentication, shell, key generation.
 */

#define BRUCE_SSH_HOST_KEY_SHA256_SIZE 32
#define BRUCE_SSH_PRIVATE_KEY_MAX_SIZE 512
#define BRUCE_SSH_PUBLIC_KEY_MAX_SIZE 256

typedef enum {
    BRUCE_SSH_KEY_ECDSA_P256 = 0,
    BRUCE_SSH_KEY_ED25519,
} bruce_ssh_key_type_t;

/**
 * @brief Connects and reads the negotiated host key.
 *
 * SSH sessions are opaque, owned by the calling process, and close
 * automatically when that process exits. The caller must verify the
 * host-key fingerprint before authenticating.
 *
 * @param host Hostname or IP address to connect to.
 * @param port TCP port to connect to.
 * @param timeout_ms Connection timeout in milliseconds.
 * @param out_session Receives the new session handle.
 * @permission ssh
 */
bruce_result_t ssh__connect(
    const char *host, uint16_t port, uint32_t timeout_ms, bruce_ssh_id_t *out_session
);

/**
 * @brief Reads the negotiated host key's SHA-256 fingerprint.
 *
 * @param session Session to query.
 * @param out_fingerprint Receives the SHA-256 fingerprint.
 * @permission ssh
 */
bruce_result_t ssh__host_key_sha256(
    bruce_ssh_id_t session, uint8_t out_fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
);

/**
 * @brief Compares an expected fingerprint to the negotiated host key.
 *
 * Authentication remains unavailable until this succeeds.
 *
 * @param session Session to verify.
 * @param expected Expected SHA-256 fingerprint to compare against.
 * @permission ssh
 */
bruce_result_t ssh__verify_host_key_sha256(
    bruce_ssh_id_t session, const uint8_t expected[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
);

/**
 * @brief Authenticates a session with a password.
 *
 * @param session Session to authenticate.
 * @param username Username to authenticate as.
 * @param password Password to authenticate with.
 * @param timeout_ms Authentication timeout in milliseconds.
 * @permission ssh
 */
bruce_result_t ssh__authenticate_password(
    bruce_ssh_id_t session, const char *username, const char *password, uint32_t timeout_ms
);

/**
 * @brief Generates the selected key type.
 *
 * Ed25519 private keys use the unencrypted OpenSSH format; ECDSA P-256
 * uses SEC1 PEM. Text sizes exclude the trailing NUL, and the caller owns
 * both buffers.
 *
 * @param type Key type to generate.
 * @param private_key Buffer to receive the generated private key text.
 * @param private_capacity Size of private_key in bytes.
 * @param out_private_size Receives the private key text length (excluding NUL).
 * @param public_key Buffer to receive the generated public key text.
 * @param public_capacity Size of public_key in bytes.
 * @param out_public_size Receives the public key text length (excluding NUL).
 * @permission ssh
 */
bruce_result_t ssh__generate_keypair_ex(
    bruce_ssh_key_type_t type, char *private_key, size_t private_capacity,
    size_t *out_private_size, char *public_key, size_t public_capacity,
    size_t *out_public_size
);

/**
 * @brief Authenticates with an unencrypted ECDSA P-256 SEC1 PEM key or Ed25519 private key.
 *
 * (or an unencrypted OpenSSH Ed25519 private key).
 *
 * @param session Session to authenticate.
 * @param username Username to authenticate as.
 * @param private_key Private key bytes (SEC1 PEM or OpenSSH format).
 * @param private_key_size Number of bytes in private_key.
 * @param timeout_ms Authentication timeout in milliseconds.
 * @permission ssh
 */
bruce_result_t ssh__authenticate_key(
    bruce_ssh_id_t session, const char *username, const void *private_key,
    size_t private_key_size, uint32_t timeout_ms
);

/**
 * @brief Opens an interactive shell channel on an authenticated session.
 *
 * @param session Authenticated session to open a shell on.
 * @param terminal_type Terminal type to request, e.g. "xterm".
 * @param columns Initial terminal width in columns.
 * @param rows Initial terminal height in rows.
 * @param timeout_ms Open timeout in milliseconds.
 * @permission ssh
 */
bruce_result_t ssh__open_shell(
    bruce_ssh_id_t session, const char *terminal_type, uint16_t columns, uint16_t rows,
    uint32_t timeout_ms
);

/**
 * @brief Notifies the remote end of a terminal size change on an open shell.
 *
 * @param session Session with an open shell.
 * @param columns New terminal width in columns.
 * @param rows New terminal height in rows.
 * @param timeout_ms Request timeout in milliseconds.
 * @permission ssh
 */
bruce_result_t ssh__resize_pty(
    bruce_ssh_id_t session, uint16_t columns, uint16_t rows, uint32_t timeout_ms
);

/**
 * @brief Reads from an SSH channel.
 *
 * EOF is BRUCE_OK with *out_size == 0. Set `stderr_stream` to read the SSH
 * channel's extended-data stream.
 *
 * @param session Session to read from.
 * @param stderr_stream If true, reads the channel's extended-data (stderr) stream.
 * @param buffer Buffer to receive read bytes.
 * @param capacity Size of buffer in bytes.
 * @param timeout_ms Read timeout in milliseconds.
 * @param out_size Receives the number of bytes read.
 * @permission ssh
 */
bruce_result_t ssh__read(
    bruce_ssh_id_t session, bool stderr_stream, void *buffer, size_t capacity,
    uint32_t timeout_ms, size_t *out_size
);

/**
 * @brief Writes to an SSH channel.
 *
 * @param session Session to write to.
 * @param buffer Bytes to write.
 * @param size Number of bytes in buffer.
 * @param timeout_ms Write timeout in milliseconds.
 * @param out_size Receives the number of bytes written.
 * @permission ssh
 */
bruce_result_t ssh__write(
    bruce_ssh_id_t session, const void *buffer, size_t size, uint32_t timeout_ms,
    size_t *out_size
);

/**
 * @brief Closes an SSH session opened by ssh__connect().
 *
 * @param session Session to close.
 * @permission ssh
 */
bruce_result_t ssh__close(bruce_ssh_id_t session);
