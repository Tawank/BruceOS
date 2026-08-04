#!/usr/bin/env python3
"""Run idf.py against one of the boards/ configurations.

Each board under boards/<id>/ contributes:
  - sdkconfig.defaults: CONFIG_BRUCE_BOARD_* selection and any other
    Kconfig overrides that need to differ per board.
  - target: the IDF_TARGET chip name (e.g. esp32, esp32s3), one line.

The root CMakeLists.txt recognizes the build-board/<id> directory name and
wires up SDKCONFIG/SDKCONFIG_DEFAULTS for that board automatically (the
same trick it already uses for build-qemu) - but only once CMake actually
runs. Before that, idf.py itself does two Python-side pre-flight checks
that this script has to satisfy on the command line, or every board build
fails or silently targets the wrong chip:

  - _check_idf_target() (idf_py_actions/tools.py) compares an IDF_TARGET
    environment variable already exported in the calling shell (e.g. from a
    previous `idf.py set-target` in that terminal) against this invocation,
    and hard-errors on any mismatch rather than picking one. So this script
    always sets IDF_TARGET in the subprocess's environment to the board's
    own target, overriding whatever the caller's shell already had.
  - That same check also resolves "the project's sdkconfig" itself, and
    without a -D SDKCONFIG override it always falls back to
    <project_dir>/sdkconfig (this repo's checked-in Cardputer/esp32s3
    config) for any build directory that hasn't been configured by CMake
    yet - it has no idea about our build-board/<id> redirect since that
    logic only exists inside CMakeLists.txt. So this script also passes
    -D SDKCONFIG=<build-dir>/sdkconfig explicitly, pointing at the same
    per-board path CMakeLists.txt will use once it runs.

Usage:
    python tools/board.py <board> [idf.py args...]
    python tools/board.py --list

Examples:
    python tools/board.py m5stack-cardputer build
    python tools/board.py m5stack-cplus2 build flash monitor -p /dev/ttyUSB0
    python tools/board.py m5stack-cplus2          # defaults to "build"
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BOARDS_DIR = REPO_ROOT / "boards"


def discover_boards() -> dict[str, Path]:
    if not BOARDS_DIR.is_dir():
        return {}
    boards = {}
    for entry in sorted(BOARDS_DIR.iterdir()):
        defaults = entry / "sdkconfig.defaults"
        if entry.is_dir() and defaults.is_file():
            boards[entry.name] = defaults
    return boards


def board_target(board_dir_name: str) -> str:
    target_file = BOARDS_DIR / board_dir_name / "target"
    if not target_file.is_file():
        print(
            f"error: boards/{board_dir_name}/target is missing (should contain the "
            "IDF_TARGET chip name, e.g. esp32 or esp32s3)",
            file=sys.stderr,
        )
        sys.exit(1)
    return target_file.read_text().strip()


def board_summary(defaults_file: Path) -> str:
    for line in defaults_file.read_text().splitlines():
        line = line.strip()
        if line.startswith("#") and line.lstrip("#").strip():
            return line.lstrip("#").strip()
    return ""


def print_board_list(boards: dict[str, Path]) -> None:
    if not boards:
        print(f"No boards found under {BOARDS_DIR}")
        return
    width = max(len(name) for name in boards)
    for name, defaults_file in boards.items():
        summary = board_summary(defaults_file)
        print(f"  {name.ljust(width)}  {summary}" if summary else f"  {name}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build Bruce for a specific board via idf.py.",
        usage="%(prog)s [-h] [--list] <board> [idf.py args...]",
    )
    parser.add_argument("--list", action="store_true", help="list available board ids and exit")
    parser.add_argument("board", nargs="?", help="board id, e.g. m5stack-cplus2")
    parser.add_argument("idf_args", nargs=argparse.REMAINDER, help="idf.py action(s)/flags; default: build")
    args = parser.parse_args()

    boards = discover_boards()

    if args.list:
        print_board_list(boards)
        return 0

    if not args.board:
        parser.print_usage()
        print("\nAvailable boards:")
        print_board_list(boards)
        return 1

    if args.board not in boards:
        print(f"error: unknown board '{args.board}'\n", file=sys.stderr)
        print("Available boards:", file=sys.stderr)
        print_board_list(boards)
        return 1

    if shutil.which("idf.py") is None:
        print(
            "error: idf.py not found on PATH. Activate ESP-IDF first, e.g.:\n"
            "  source ~/.espressif/v6.0.2/esp-idf/export.sh",
            file=sys.stderr,
        )
        return 1

    build_dir = REPO_ROOT / "build-board" / args.board
    target = board_target(args.board)
    idf_actions = args.idf_args or ["build"]

    command = [
        "idf.py",
        "-C",
        str(REPO_ROOT),
        "-B",
        str(build_dir),
        "-D",
        f"SDKCONFIG={build_dir / 'sdkconfig'}",
        *idf_actions,
    ]
    env = dict(os.environ)
    env["IDF_TARGET"] = target
    print(f"+ IDF_TARGET={target} {' '.join(command)}")
    return subprocess.call(command, env=env)


if __name__ == "__main__":
    sys.exit(main())
