#!/usr/bin/env python3
"""Regenerates docs/COMMANDS.md from the firmware's own `man --gen-md`.

Thin wrapper around `pytest tests/pytest_gen_docs.py`: that file does the
actual work (reconfigure+rebuild build-qemu/ to boot straight into
`man --gen-md`, then capture and clean up its output) by reusing the exact
QEMU-boot-and-serial-capture plumbing tests/pytest_firmware.py already
relies on, rather than a second copy of it here.

Prerequisites: an ESP-IDF environment sourced (`idf.py` on PATH) -- the
rebuild with the right boot command happens automatically, so an existing
build-qemu/ doesn't need to already match the current source.

Run from the repo root:
    python tools/gen_commands_doc.py
"""
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
COMMANDS_MD = REPO_ROOT / "docs" / "COMMANDS.md"


def main() -> int:
    result = subprocess.run(
        [sys.executable, "-m", "pytest", "tests/pytest_gen_docs.py", "-s"],
        cwd=REPO_ROOT,
    )
    if result.returncode != 0:
        print("gen_commands_doc: failed -- see pytest output above", file=sys.stderr)
        return result.returncode
    print(f"gen_commands_doc: wrote {COMMANDS_MD}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
