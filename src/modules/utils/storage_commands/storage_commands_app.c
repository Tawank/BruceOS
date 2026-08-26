#include "storage_commands_app.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/* The spiritual successor to BrucePIO_legacy's "storage" composite CLI
 * (src/core/serial_commands/storage_commands.cpp): same "storage <verb>
 * <filepath> [...]" shape, meant to be typed by hand exactly like the old
 * one was, but ported to this repo's idioms rather than translated
 * line-for-line.
 *
 * "list"/"remove"/"mkdir" don't reimplement any storage logic here - they
 * just forward to the standalone "ls"/"rm"/"mkdir" commands via
 * app_runner__run() and pass their exit code through, so path resolution
 * (relative to the shell's working directory) and error messages stay
 * identical to running those commands directly. "rename" has no standalone
 * command to forward to, so it calls storage__rename() itself.
 *
 * "read"/"write" are the one part with no legacy equivalent worth keeping:
 * legacy's non-Y-modem "write" read the incoming data with
 * String::readStringUntil('\n') and an "EOF" sentinel line - fine for text,
 * but it truncates at the first embedded '\0' and cannot represent a file
 * that legitimately contains a line reading "EOF". Y-modem solved that
 * properly but is a lot of protocol for what a same-repo host tool (see
 * tools/esp_storage.py) needs. Here we lean on something the legacy Arduino
 * stack didn't have: stdio__read()/write() already round-trip arbitrary
 * bytes with no line buffering, no CR/LF translation, and no flow-control
 * byte-stuffing (see core/stdio/stdio.c - the physical console path is a
 * thin wrapper over usb_serial_jtag_read_bytes()/write_bytes()), *as long as
 * the transfer isn't itself going through a shell pipe/`$()` capture session
 * (see shell_executor.c), which "storage read"/"storage write" typed or run
 * plainly at the prompt never is. So the length-prefixed raw-byte protocol
 * below just works, with no escaping needed in either direction.
 *
 * Both directions start with the size on a marker line - write is told the
 * size up front (like legacy's "write"), read has to discover it (via
 * SEEK_END) and report it before the first data byte - so the host side
 * never has to guess when the payload ends by watching for a timeout, and
 * "the file's actual bytes happen to look like a sentinel" can't happen: the
 * marker line starts with 0x02 (STX), a byte the shell's line editor never
 * echoes back for typed input (it only inserts/redraws printable ' '..'~')
 * and one vanishingly unlikely to open any other line this app or the shell
 * prints, so a host scanning the byte stream for it doesn't need to parse
 * ANSI cursor movement or prompt redraws to find it. */

#define STORAGE_COMMANDS__CHUNK_SIZE 512u
#define STORAGE_COMMANDS__MARKER "\x02STORAGE"

static void storage_commands__usage_error(const char *message) { stdio__printf("storage: %s\n", message); }

static bool storage_commands__require_absolute(const char *path) {
    if (path != NULL && path[0] == '/') return true;
    storage_commands__usage_error("path must be absolute (start with '/')");
    return false;
}

static bool storage_commands__parse_size(const char *text, size_t *out_size) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == NULL || *end != '\0') return false;
    *out_size = (size_t)value;
    return true;
}

/* Wraps `value` in double quotes, escaping '"' and '\\', so it survives
 * app_runner__parse_args()'s tokenizer as a single argument no matter what
 * it contains (spaces included). Returns false if `out` (size `capacity`)
 * is too small, rather than truncating a path. */
static bool storage_commands__quote_arg(const char *value, char *out, size_t capacity) {
    if (capacity < 3) return false;
    size_t pos = 0;
    out[pos++] = '"';
    for (const char *p = value; *p != '\0'; ++p) {
        size_t needed = (*p == '"' || *p == '\\') ? 2 : 1;
        if (pos + needed + 2 > capacity) return false; /* room for closing '"' and '\0' too */
        if (needed == 2) out[pos++] = '\\';
        out[pos++] = *p;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return true;
}

/* Runs a standalone built-in command with a single already-quoted argument
 * and passes its exit code through, the way a shell would. */
static int storage_commands__forward(const char *app_name, const char *arg) {
    int process_id = app_runner__run(app_name, arg, BRUCE_LAUNCH_BACKGROUND);
    if (process_id < 0) return process_id;
    bruce_process_status_t status;
    bruce_result_t wait_result =
        process__wait_status((bruce_process_id_t)process_id, UINT32_MAX, &status);
    if (wait_result != BRUCE_OK) return wait_result;
    return status.exit_code;
}

static int storage_commands__forward_path(const char *app_name, const char *path) {
    if (path == NULL) return storage_commands__forward(app_name, NULL);
    char quoted[BRUCE_STORAGE_PATH_MAX * 2 + 4];
    if (!storage_commands__quote_arg(path, quoted, sizeof(quoted))) {
        storage_commands__usage_error("path is too long");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return storage_commands__forward(app_name, quoted);
}

static int storage_commands__rename(const char *path, const char *new_path) {
    if (!storage_commands__require_absolute(path) || !storage_commands__require_absolute(new_path)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = storage__rename(path, new_path);
    if (result != BRUCE_OK) {
        stdio__printf("storage: rename %s to %s: error %d\n", path, new_path, (int)result);
    }
    return result;
}

/* Reads exactly `size` bytes from stdin and writes them to `path`, telling
 * the host how many bytes to send right before reading the first one so it
 * never has to send a length it hopes the firmware already agrees on.
 *
 * Each chunk is acknowledged (WRITE-CHUNK-OK) before the loop reads again,
 * and the host (tools/esp_storage.py) waits for that ack before sending its
 * next chunk. That's load-bearing, not just informational progress: the
 * ESP32's usb_serial_jtag RX ISR (usb_serial_jtag.c) hands each incoming USB
 * packet to a fixed-size ring buffer (1024 bytes - see stdio__init()) via
 * xRingbufferSendFromISR() *without checking whether that succeeded*. USB
 * itself never sees a problem - the ISR already pulled the bytes off the
 * hardware FIFO and acked the packet - so there's no NAK/backpressure; if
 * the host ever gets far enough ahead of this loop (draining a chunk here
 * includes a flash write, which is slow) for the ring buffer to fill, every
 * further byte is silently dropped on the floor and this call hangs forever
 * waiting for bytes that already came and went. A fixed host-side send
 * delay was tried instead of this and didn't hold up - flash write latency
 * isn't a constant worth guessing at. Acking per chunk means the host is
 * structurally incapable of getting more than one chunk ahead, independent
 * of how slow any given flash write turns out to be. */
static int storage_commands__write(const char *path, size_t size) {
    if (!storage_commands__require_absolute(path)) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) {
        stdio__printf(STORAGE_COMMANDS__MARKER " WRITE-ERR %d\n", (int)result);
        return result;
    }

    stdio__printf(STORAGE_COMMANDS__MARKER " WRITE-READY %zu\n", size);

    uint8_t chunk[STORAGE_COMMANDS__CHUNK_SIZE];
    size_t received = 0;
    while (result == BRUCE_OK && received < size) {
        size_t want = size - received;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        size_t read_size = 0;
        result = stdio__read(chunk, want, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result != BRUCE_OK) break;

        size_t written = 0;
        result = storage__write(file, chunk, read_size, &written);
        if (result == BRUCE_OK && written != read_size) result = BRUCE_ERR_IO;
        received += read_size;
        if (result == BRUCE_OK) stdio__printf(STORAGE_COMMANDS__MARKER " WRITE-CHUNK-OK %zu\n", received);
    }

    bruce_result_t close_result = storage__close(file);
    if (result == BRUCE_OK) result = close_result;

    if (result != BRUCE_OK) {
        stdio__printf(STORAGE_COMMANDS__MARKER " WRITE-ERR %d\n", (int)result);
        return result;
    }
    stdio__printf(STORAGE_COMMANDS__MARKER " WRITE-OK %zu\n", received);
    return BRUCE_OK;
}

/* Mirrors storage_commands__write() the other way: reports the file's size
 * up front, then streams the raw bytes out over stdout. */
static int storage_commands__read(const char *path) {
    if (!storage_commands__require_absolute(path)) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) {
        stdio__printf(STORAGE_COMMANDS__MARKER " READ-ERR %d\n", (int)result);
        return result;
    }

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);
    if (result != BRUCE_OK) {
        storage__close(file);
        stdio__printf(STORAGE_COMMANDS__MARKER " READ-ERR %d\n", (int)result);
        return result;
    }

    stdio__printf(STORAGE_COMMANDS__MARKER " READ-READY %llu\n", (unsigned long long)size);

    uint8_t chunk[STORAGE_COMMANDS__CHUNK_SIZE];
    uint64_t sent = 0;
    while (result == BRUCE_OK && sent < size) {
        size_t read_size = 0;
        result = storage__read(file, chunk, sizeof(chunk), &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result != BRUCE_OK) break;
        result = stdio__write(chunk, read_size);
        sent += read_size;
    }

    bruce_result_t close_result = storage__close(file);
    if (result == BRUCE_OK) result = close_result;

    if (result != BRUCE_OK) {
        stdio__printf(STORAGE_COMMANDS__MARKER " READ-ERR %d\n", (int)result);
        return result;
    }
    stdio__printf(STORAGE_COMMANDS__MARKER " READ-OK %llu\n", (unsigned long long)sent);
    return BRUCE_OK;
}

int storage_commands_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(
        root,
        "Manage files over the same interface BrucePIO's legacy 'storage' command did. "
        "'list'/'remove'/'mkdir' forward to 'ls'/'rm'/'mkdir'; 'rename' renames in place; "
        "'read'/'write' transfer a file's raw bytes to/from stdout/stdin (for tools/esp_storage.py)."
    );

    ArgParser *list_cmd = ap_new_cmd(root, "list");
    ArgParser *remove_cmd = ap_new_cmd(root, "remove");
    ArgParser *mkdir_cmd = ap_new_cmd(root, "mkdir");
    ArgParser *rename_cmd = ap_new_cmd(root, "rename");
    ArgParser *write_cmd = ap_new_cmd(root, "write");
    ArgParser *read_cmd = ap_new_cmd(root, "read");
    if (list_cmd == NULL || remove_cmd == NULL || mkdir_cmd == NULL || rename_cmd == NULL
        || write_cmd == NULL || read_cmd == NULL) {
        ap_free(root);
        return BRUCE_ERR_NO_MEMORY;
    }

    ap_set_helptext(list_cmd, "List a directory's contents (see 'ls').");
    ap_add_optional_arg(list_cmd, "path", "Path to list (defaults to the working directory)");

    ap_set_helptext(remove_cmd, "Remove a file or empty directory (see 'rm').");
    ap_add_required_arg(remove_cmd, "path", "Path to remove");

    ap_set_helptext(mkdir_cmd, "Create a directory (see 'mkdir').");
    ap_add_required_arg(mkdir_cmd, "path", "Directory path to create");

    ap_set_helptext(rename_cmd, "Rename or move a file or directory.");
    ap_add_required_arg(rename_cmd, "path", "Absolute source path");
    ap_add_required_arg(rename_cmd, "new_path", "Absolute destination path");

    ap_set_helptext(write_cmd, "Read <size> raw bytes from stdin and save them to <path>.");
    ap_add_required_arg(write_cmd, "path", "Absolute destination path");
    ap_add_required_arg(write_cmd, "size", "Exact number of bytes to read");

    ap_set_helptext(read_cmd, "Stream <path>'s raw bytes to stdout.");
    ap_add_required_arg(read_cmd, "path", "Absolute source path");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t parse_status = ap_get_status(root);
        ap_free(root);
        return parse_status == AP_STATUS_HELP || parse_status == AP_STATUS_VERSION
                   ? BRUCE_OK
                   : BRUCE_ERR_INVALID_ARGUMENT;
    }

    int result = BRUCE_ERR_INVALID_ARGUMENT;
    ArgParser *command = ap_get_cmd_parser(root);
    if (command == list_cmd) {
        result = storage_commands__forward_path("ls", ap_get_arg(list_cmd, "path"));
    } else if (command == remove_cmd) {
        result = storage_commands__forward_path("rm", ap_get_arg(remove_cmd, "path"));
    } else if (command == mkdir_cmd) {
        result = storage_commands__forward_path("mkdir", ap_get_arg(mkdir_cmd, "path"));
    } else if (command == rename_cmd) {
        result = storage_commands__rename(ap_get_arg(rename_cmd, "path"), ap_get_arg(rename_cmd, "new_path"));
    } else if (command == write_cmd) {
        size_t size = 0;
        if (!storage_commands__parse_size(ap_get_arg(write_cmd, "size"), &size)) {
            storage_commands__usage_error("size must be a non-negative integer");
        } else {
            result = storage_commands__write(ap_get_arg(write_cmd, "path"), size);
        }
    } else if (command == read_cmd) {
        result = storage_commands__read(ap_get_arg(read_cmd, "path"));
    } else {
        storage_commands__usage_error("expected 'list', 'remove', 'mkdir', 'rename', 'write', or 'read'");
    }

    ap_free(root);
    return result;
}
