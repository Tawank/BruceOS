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
    python tools/board.py --set-default <board>

Examples:
    python tools/board.py m5stack-cardputer build
    python tools/board.py m5stack-cplus2 build flash monitor -p /dev/ttyUSB0
    python tools/board.py m5stack-cplus2          # defaults to "build"
    python tools/board.py --set-default m5stack-cplus2

--set-default makes a board the project-wide default instead of building it
into its own build-board/<id> directory: it regenerates the root sdkconfig
(and the root build/ dir) from that board's sdkconfig.defaults, which is
what a bare `idf.py build` (e.g. the VS Code ESP-IDF extension's build
button) and IntelliSense (.clangd points CompilationDatabase at build/, so
build/compile_commands.json is what clangd reads) both use. It also syncs
the personal, gitignored .vscode/settings.json's idf.customExtraVars.
IDF_TARGET and idf.openOcdConfigs target file to match, so the ESP-IDF
extension doesn't fight the new default the way IDF_TARGET env var
mismatches did for build-board/<id> builds (see below). The regenerated
root sdkconfig/sdkconfig.old are ordinary tracked files - commit them to
make the new default apply for everyone who clones the repo.
"""

import argparse
import json
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


def update_vscode_intellisense(target: str) -> None:
    """Point the personal (gitignored) VS Code ESP-IDF/OpenOCD target at `target`.

    Best-effort: does nothing if .vscode/settings.json doesn't exist, since
    it's a per-developer file this repo doesn't check in.
    """
    settings_file = REPO_ROOT / ".vscode" / "settings.json"
    if not settings_file.is_file():
        return
    data = json.loads(settings_file.read_text())

    extra_vars = data.setdefault("idf.customExtraVars", {})
    extra_vars["IDF_TARGET"] = target

    ocd_configs = data.get("idf.openOcdConfigs")
    if isinstance(ocd_configs, list):
        data["idf.openOcdConfigs"] = [
            f"target/{target}.cfg" if entry.startswith("target/") else entry
            for entry in ocd_configs
        ]

    settings_file.write_text(json.dumps(data, indent=2) + "\n")
    print(f"+ updated .vscode/settings.json for IDF_TARGET={target}")


def set_default(board: str) -> int:
    target = board_target(board)
    defaults = f"sdkconfig.defaults;boards/{board}/sdkconfig.defaults"

    # Wipe the previous default's generated state so set-target regenerates the
    # root sdkconfig purely from these defaults, instead of merging on top of
    # whatever board used to be the default.
    for stale in (REPO_ROOT / "build", REPO_ROOT / "sdkconfig", REPO_ROOT / "sdkconfig.old"):
        if stale.is_dir():
            shutil.rmtree(stale)
        elif stale.is_file():
            stale.unlink()

    command = [
        "idf.py",
        "-C",
        str(REPO_ROOT),
        "-D",
        f"SDKCONFIG_DEFAULTS={defaults}",
        "set-target",
        target,
    ]
    env = dict(os.environ)
    env["IDF_TARGET"] = target
    print(f"+ IDF_TARGET={target} {' '.join(command)}")
    ret = subprocess.call(command, env=env)
    if ret == 0:
        update_vscode_intellisense(target)
        print(f"\n'{board}' is now the project-wide default (root sdkconfig + build/).")
        print("Review and commit the regenerated sdkconfig/sdkconfig.old to make this")
        print("the default for everyone who clones the repo.")
    return ret


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build Bruce for a specific board via idf.py.",
        usage="%(prog)s [-h] [--list] <board> [idf.py args...]",
    )
    parser.add_argument("--list", action="store_true", help="list available board ids and exit")
    parser.add_argument(
        "--set-default",
        action="store_true",
        help=(
            "make <board> the project-wide default: regenerate the root sdkconfig "
            "and build/ dir (used by a bare `idf.py build` and by IntelliSense) "
            "instead of building into build-board/<id>"
        ),
    )
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

    if args.set_default:
        return set_default(args.board)

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
