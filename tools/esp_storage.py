#!/usr/bin/env python3
"""Talk to a running BruceIDF device's "storage" command over its USB serial
console, the way a person would type at the interactive shell -- just
scripted, and with a real binary-safe channel for actual file transfer.

This is the host-side half of src/modules/utils/storage_commands/ (see that
module's header comment for the wire protocol and why it exists); together
they're the spiritual successor to BrucePIO_legacy's "storage" serial CLI
(src/core/serial_commands/storage_commands.cpp), minus its Y-modem path,
which a same-repo host tool doesn't need.

Usage:
    tools/esp_storage.py put native_apps/examples/game3d.elf /apps/game3d.elf
    tools/esp_storage.py get /apps/game3d.elf ./game3d.elf
    tools/esp_storage.py list /apps
    tools/esp_storage.py mkdir /apps
    tools/esp_storage.py rename /apps/old.elf /apps/new.elf
    tools/esp_storage.py remove /apps/game3d.elf

All subcommands take -p/--port (default: $ESPPORT) and -b/--baud (default:
115200 -- irrelevant to the ESP32's built-in USB-Serial-JTAG peripheral,
which is a real USB CDC device with no UART to clock, but pyserial still
wants a value).

How this synchronizes with the shell
-------------------------------------
core/serial_commands (serial_commands_app_main) doesn't start the
interactive shell until it sees a first byte on the console, and discards
that byte rather than feeding it to the shell -- so opening the port and
sending one newline is how this script wakes a freshly booted device up,
same as pressing a key in a terminal would. That is also harmless to send
to a shell that's already running (it just submits an empty command line),
so this script always does it and then waits for a fresh copy of the
prompt (see PROMPT below) before doing anything else, rather than trying to
tell "cold boot" and "already running" apart.

Once synchronized, `put`/`get` drive "storage write"/"storage read" (see
storage_commands_app.c for the marker-line protocol) for real binary
transfer. `list`/`remove`/`mkdir`/`rename` just forward to
"storage list"/"remove"/"mkdir"/"rename", which are themselves forwarding
wrappers around "ls"/"rm"/"mkdir" (or storage__rename() directly) on the
device -- there's no marker protocol for those, so this script recovers
their output the same way any dumb terminal would: it types the command,
then reads and prints raw bytes until the next prompt reappears. There's no
machine-readable success/failure signal for that path (the device doesn't
send one), so those subcommands always exit 0 here; read the printed output
to tell whether they actually worked, same as you would watching a real
terminal.

Watching the raw traffic
-------------------------
Since this script has the port open, a separate `idf.py monitor` can't also
attach to watch what the device is actually saying (e.g. a crash's own
"Guru Meditation Error" backtrace, printed on the same console right before
it reboots). So every byte this script reads gets echoed, unbuffered and
unmodified (ANSI codes included - a real terminal renders those the way
`idf.py monitor` would), to stderr as it arrives, not just quoted back in an
error message afterward. That's on by default, since seeing what actually
happened is the point when something's gone wrong; pass -q/--quiet to turn
it off once a workflow is known to work and the raw traffic is just noise.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print(
        "error: this tool needs pyserial (pip install pyserial); it's already a "
        "dependency of the ESP-IDF environment used to build/flash, so "
        "`source /opt/esp/idf/export.sh` (or your IDF export script) before "
        "running this may be all that's needed.",
        file=sys.stderr,
    )
    sys.exit(1)

DEFAULT_BAUD = 115200
MARKER = b"\x02STORAGE"
MARKER_RE = re.compile(rb"\x02STORAGE ([A-Z-]+)(?: (-?\d+))?\r?\n")
# Mirrors SHELL_CONSOLE_PROMPT in src/modules/shell/shell_console.c exactly;
# if that constant changes, this needs to change with it.
PROMPT = b"\r\033[0m\033[2K\033[1;36mbruce\033[0m$ "
CHUNK_SIZE = 4096
# See the send loop in put() for why a payload isn't just one ser.write()
# call. Matches storage_commands_app.c's own STORAGE_COMMANDS__CHUNK_SIZE
# (the amount it reads-then-flash-writes per loop iteration); there's no
# benefit sending more than the device consumes before it acks.
STORAGE_COMMANDS_CHUNK_SIZE = 512


class StorageError(Exception):
    """A "storage" command failed, or the device didn't respond as expected."""


ECHO = True  # toggled by -q/--quiet in main(); see the module docstring


def _read_chunk(ser: "serial.Serial", size: int, buf: bytearray, echo: bool = True) -> bytes:
    """The one place that actually calls ser.read(): appends whatever came
    back to `buf` and, unless silenced, echoes it to stderr immediately and
    unmodified -- this is what lets a crash's own backtrace show up here
    instead of needing a separate `idf.py monitor` this script has the port
    open against. `echo=False` (used for `get`'s actual file payload, not
    its marker lines) is not about -q/--quiet; it's because dumping a
    binary file's raw bytes at a terminal can contain control sequences
    that visibly mangle it, which -q isn't meant to protect against."""
    chunk = ser.read(size)
    if chunk:
        buf.extend(chunk)
        if ECHO and echo:
            sys.stderr.buffer.write(chunk)
            sys.stderr.buffer.flush()
    return chunk


def _read_until(ser: "serial.Serial", needle: bytes, deadline: float, buf: bytearray) -> bytes:
    """Reads into `buf` until it contains `needle`, returning everything up to
    and including it (and leaving anything after it still in `buf`)."""
    while True:
        index = buf.find(needle)
        if index >= 0:
            end = index + len(needle)
            found = bytes(buf[:end])
            del buf[:end]
            return found
        if time.monotonic() >= deadline:
            raise StorageError(
                f"timed out waiting for {needle!r}; got so far: {bytes(buf)!r}"
            )
        _read_chunk(ser, CHUNK_SIZE, buf)


def _read_marker(ser: "serial.Serial", deadline: float, buf: bytearray) -> tuple[str, int | None]:
    """Reads up to and past the next "\\x02STORAGE ..." marker line, returning
    (verb, value) -- e.g. ("WRITE-READY", 1234) or ("WRITE-ERR", -5)."""
    while True:
        match = MARKER_RE.search(buf)
        if match is not None:
            # Pull the groups out before mutating `buf`: a match against a
            # bytearray keeps a reference to it rather than a copy, so
            # group() re-slices whatever `buf` currently holds using the
            # match's stored offsets -- del'ing the matched prefix first
            # shifts everything left underneath those offsets and group()
            # then returns nonsense (usually b'', past the shrunk end).
            verb = match.group(1).decode()
            raw_value = match.group(2)
            value = int(raw_value) if raw_value is not None else None
            del buf[: match.end()]
            return verb, value
        if time.monotonic() >= deadline:
            raise StorageError(f"timed out waiting for a STORAGE marker; got so far: {bytes(buf)!r}")
        # Read byte-at-a-time near a possible '\x02' so we don't block past a
        # marker that's only partially arrived; small reads are fine here,
        # this only runs while waiting for a short status line.
        _read_chunk(ser, 1, buf)


def _read_exact(ser: "serial.Serial", size: int, deadline: float, buf: bytearray, echo: bool = True) -> bytes:
    """Returns exactly `size` bytes, taking any already-buffered bytes first."""
    while len(buf) < size:
        if time.monotonic() >= deadline:
            raise StorageError(f"timed out reading payload: got {len(buf)} of {size} bytes")
        _read_chunk(ser, min(CHUNK_SIZE, size - len(buf)), buf, echo=echo)
    data = bytes(buf[:size])
    del buf[:size]
    return data


def connect(port: str, baud: int, timeout: float) -> tuple["serial.Serial", bytearray]:
    """Opens the port, wakes/synchronizes with the shell, and returns it
    along with the (possibly non-empty) leftover read buffer."""
    ser = serial.Serial(port, baud, timeout=0.1)
    # pyserial asserts both DTR and RTS by default on open. On boards whose
    # auto-reset circuit (or the ESP32-S3's native USB-Serial-JTAG
    # equivalent of it) wires those lines to EN/boot-select, that holds the
    # chip in reset the whole time -- which looks exactly like "the device
    # never sends a single byte back", not like a protocol error. idf.py
    # monitor/esptool release both for the same reason before talking to a
    # board that's just supposed to be running; do the same here, and give
    # the board a moment to finish booting in case releasing them was itself
    # a reset edge.
    ser.dtr = False
    ser.rts = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    buf = bytearray()
    ser.write(b"\n")
    ser.flush()
    _read_until(ser, PROMPT, time.monotonic() + timeout, buf)
    return ser, buf


def run_command(ser: "serial.Serial", buf: bytearray, command: str, timeout: float) -> None:
    ser.write(command.encode() + b"\n")
    ser.flush()


# A floor on how long to wait for a large transfer to actually finish, in
# bytes/second: `--timeout` is sized for quick control-plane exchanges (a
# prompt, a status line), but the device is writing/reading real flash
# between marker lines for `put`/`get`, and how long that takes isn't
# something this script can know in advance. 4 KiB/s is deliberately
# pessimistic (a slow-flash worst case, not a target), so a transfer that's
# just taking a while doesn't get killed at the same timeout a stuck one
# would also hit.
TRANSFER_MIN_BYTES_PER_SEC = 2048


def _transfer_deadline(timeout: float, size: int) -> float:
    return time.monotonic() + max(timeout, size / TRANSFER_MIN_BYTES_PER_SEC)


def put(ser: "serial.Serial", buf: bytearray, local_file: str, remote_path: str, timeout: float) -> None:
    if not remote_path.startswith("/"):
        raise StorageError(f"remote path must be absolute (start with '/'): {remote_path!r}")
    data = Path(local_file).read_bytes()

    run_command(ser, buf, f"storage write {remote_path} {len(data)}", timeout)
    deadline = time.monotonic() + timeout
    verb, value = _read_marker(ser, deadline, buf)
    if verb != "WRITE-READY":
        raise StorageError(f"storage write: expected WRITE-READY, got {verb} {value}")
    if value != len(data):
        raise StorageError(f"storage write: device expects {value} bytes, we have {len(data)}")

    # Sent as chunks, each one held back until the device acks the last one
    # (storage_commands_app.c's WRITE-CHUNK-OK) rather than as one
    # ser.write(data) burst -- this is load-bearing, not just a progress
    # readout. The ESP32's usb_serial_jtag driver ISR (usb_serial_jtag.c)
    # hands each incoming USB packet to a small (1024-byte, see
    # core/stdio/stdio.c) ring buffer via xRingbufferSendFromISR() *without
    # checking whether that succeeded*. USB itself sees nothing wrong -- the
    # ISR already pulled the bytes off the hardware FIFO and acked the
    # packet -- so there's no NAK/backpressure; if this script ever gets far
    # enough ahead of the device's read-then-flash-write loop for that
    # buffer to fill, every further byte is silently dropped and the device
    # hangs forever waiting for bytes that already came and went. A fixed
    # delay between sends was tried instead of this and didn't hold up --
    # flash write latency isn't a constant worth guessing at. Waiting for an
    # explicit ack makes it impossible to get more than one chunk ahead,
    # regardless of how slow any given flash write turns out to be.
    sent = 0
    acked = 0
    while sent < len(data):
        end = min(sent + STORAGE_COMMANDS_CHUNK_SIZE, len(data))
        ser.write(data[sent:end])
        ser.flush()
        sent = end
        # The device acks its own internal read chunks, which don't
        # necessarily line up 1:1 with what was just sent (a single
        # stdio__read() can come back with less than it asked for) -- so
        # keep collecting acks until they've caught up to what we sent,
        # rather than assuming exactly one ack per send.
        while acked < sent:
            verb, value = _read_marker(ser, _transfer_deadline(timeout, sent - acked), buf)
            if verb != "WRITE-CHUNK-OK":
                raise StorageError(f"storage write: expected WRITE-CHUNK-OK, got {verb} {value}")
            acked = value

    verb, value = _read_marker(ser, time.monotonic() + timeout, buf)
    if verb != "WRITE-OK":
        raise StorageError(f"storage write failed: {verb} {value}")
    if value != len(data):
        raise StorageError(f"storage write: device only wrote {value} of {len(data)} bytes")
    print(f"wrote {value} bytes to {remote_path}")


def get(ser: "serial.Serial", buf: bytearray, remote_path: str, local_file: str, timeout: float) -> None:
    if not remote_path.startswith("/"):
        raise StorageError(f"remote path must be absolute (start with '/'): {remote_path!r}")

    run_command(ser, buf, f"storage read {remote_path}", timeout)
    deadline = time.monotonic() + timeout
    verb, value = _read_marker(ser, deadline, buf)
    if verb != "READ-READY":
        raise StorageError(f"storage read: expected READ-READY, got {verb} {value}")
    size = value

    # echo=False: this is the file's raw bytes, not console traffic -- see
    # _read_chunk()'s docstring for why those don't belong on the terminal.
    data = _read_exact(ser, size, _transfer_deadline(timeout, size), buf, echo=False)

    verb, value = _read_marker(ser, time.monotonic() + timeout, buf)
    if verb != "READ-OK":
        raise StorageError(f"storage read failed: {verb} {value}")
    if value != size:
        raise StorageError(f"storage read: device says it sent {value} of {size} bytes")

    Path(local_file).write_bytes(data)
    print(f"read {len(data)} bytes from {remote_path} -> {local_file}")


_ANSI_RE = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def relay_command(ser: "serial.Serial", buf: bytearray, command: str, timeout: float) -> None:
    """Sends a plain (non-marker) "storage" subcommand and prints whatever it
    printed, the same way watching a terminal would. See the module
    docstring for why there's no exit code here."""
    run_command(ser, buf, f"storage {command}", timeout)
    deadline = time.monotonic() + timeout
    captured = _read_until(ser, PROMPT, deadline, buf)
    captured = captured[: -len(PROMPT)]

    # Everything up to the shell's own "\r\n" (written once, right when Enter
    # is processed - see shell_console__read_line()) is just per-keystroke
    # line-editor echo of the command we just typed; the command's actual
    # output is plain text after that, with no editor redraws of its own.
    split = captured.find(b"\r\n")
    output = captured[split + 2 :] if split >= 0 else captured
    output = _ANSI_RE.sub(b"", output)
    text = output.decode(errors="replace").strip("\r\n")
    if text:
        print(text)


def main() -> int:
    # -p/-b/-t are defined once here and reused via `parents=` on every
    # subparser below, so each one takes them *after* the verb (matching how
    # this got tried in practice: "esp_storage.py put -p /dev/ttyACM0 a b").
    # They're deliberately not also added to the top-level parser: argparse
    # repopulates a subparser's own defaults into the shared namespace even
    # when its copy of a `parents=`-shared flag wasn't given again, which
    # would silently clobber a value the top-level parser had just set from
    # a "before the verb" occurrence -- one parser owning these avoids that.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-p", "--port", default=os.environ.get("ESPPORT"), help="Serial port (default: $ESPPORT)")
    common.add_argument("-b", "--baud", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default: {DEFAULT_BAUD})")
    common.add_argument(
        "-t", "--timeout", type=float, default=10.0, help="Seconds to wait for each response (default: 10)"
    )
    common.add_argument(
        "-q", "--quiet", action="store_true", help="Don't echo raw device traffic to stderr as it arrives"
    )

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="action", required=True)

    put_parser = subparsers.add_parser("put", help="Copy a local file to the device", parents=[common])
    put_parser.add_argument("local_file")
    put_parser.add_argument("remote_path")

    get_parser = subparsers.add_parser("get", help="Copy a file from the device", parents=[common])
    get_parser.add_argument("remote_path")
    get_parser.add_argument("local_file")

    list_parser = subparsers.add_parser("list", help="List a directory on the device", parents=[common])
    list_parser.add_argument("path", nargs="?", default=None)

    remove_parser = subparsers.add_parser(
        "remove", help="Remove a file or empty directory on the device", parents=[common]
    )
    remove_parser.add_argument("path")

    mkdir_parser = subparsers.add_parser("mkdir", help="Create a directory on the device", parents=[common])
    mkdir_parser.add_argument("path")

    rename_parser = subparsers.add_parser(
        "rename", help="Rename/move a file or directory on the device", parents=[common]
    )
    rename_parser.add_argument("path")
    rename_parser.add_argument("new_path")

    args = parser.parse_args()

    global ECHO
    ECHO = not args.quiet

    if not args.port:
        print("error: no serial port given (use -p/--port or set $ESPPORT)", file=sys.stderr)
        return 1
    if args.action == "put" and not Path(args.local_file).is_file():
        print(f"error: no such file: {args.local_file}", file=sys.stderr)
        return 1

    try:
        ser, buf = connect(args.port, args.baud, args.timeout)
    except serial.SerialException as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except StorageError as error:
        print(
            f"error: {error}\n"
            "(no bytes came back at all -- double check this is the console port, not a "
            "JTAG-only one if the board exposes both, and that the device is actually "
            "running and not sitting at a bootloader/reset prompt)",
            file=sys.stderr,
        )
        return 1

    try:
        if args.action == "put":
            put(ser, buf, args.local_file, args.remote_path, args.timeout)
        elif args.action == "get":
            get(ser, buf, args.remote_path, args.local_file, args.timeout)
        elif args.action == "list":
            relay_command(ser, buf, f"list {args.path}" if args.path else "list", args.timeout)
        elif args.action == "remove":
            relay_command(ser, buf, f"remove {args.path}", args.timeout)
        elif args.action == "mkdir":
            relay_command(ser, buf, f"mkdir {args.path}", args.timeout)
        elif args.action == "rename":
            relay_command(ser, buf, f"rename {args.path} {args.new_path}", args.timeout)
    except (serial.SerialException, StorageError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
