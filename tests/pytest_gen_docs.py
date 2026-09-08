"""Regenerates docs/COMMANDS.md by booting the QEMU firmware straight into
`man --gen-md`.

This isn't a correctness test -- it doesn't assert anything about firmware
behavior, it captures --gen-md's output to a file. It's marked `docs` so
conftest.py's `_reconfigure_boot_command` autouse fixture rebuilds
build-qemu/ with CONFIG_BRUCE_QEMU_TEST_BOOT_COMMAND="man --gen-md" before
this test's `dut` fixture boots it -- main.c launches that command itself
right after "QEMU READY" (there's no way to type it into an already-booted
shell; see conftest.py's docstring on _ensure_boot_command for why).

Its filename starts with `pytest_` rather than `test_`/`*_test` on purpose
(see pytest.ini: default collection only picks up `test_*.py`/`*_test.py`),
so a plain `pytest` or `pytest tests/` run never touches it; run it
explicitly:

    python -m pytest tests/pytest_gen_docs.py -s

or via `python tools/gen_commands_doc.py`, a thin wrapper around that same
command.
"""
import re
from pathlib import Path

import pytest
from pytest_embedded import Dut
from pytest_embedded.utils import remove_asci_color_code

COMMANDS_MD = Path(__file__).parents[1] / "docs" / "COMMANDS.md"
DOC_START = "# BruceOS Command Reference"
END_MARKER = b"<!-- man --gen-md: end -->"
# Generous: man --gen-md runs every registered command's --help in turn, and
# a command that ignores --help and blocks gets a 5s timeout of its own
# (MAN_APP_GEN_MD_HELP_TIMEOUT_MS) before it's killed and skipped.
GEN_MD_TIMEOUT_S = 180


@pytest.mark.docs
def test_generate_commands_doc(dut: Dut) -> None:
    dut.expect_exact("QEMU READY", timeout=60)

    captured = dut.expect(
        re.compile(re.escape(END_MARKER)), timeout=GEN_MD_TIMEOUT_S, return_what_before_match=True
    )
    text = remove_asci_color_code(captured)

    # `captured` also holds the shell's own prompt-redraw ANSI noise
    # (stripped above) before --gen-md's real output starts -- anchor on its
    # own fixed first line instead of trying to parse that away.
    start = text.index(DOC_START)
    doc = text[start:].rstrip("\n") + "\n"

    COMMANDS_MD.write_text(doc, encoding="utf-8")
    print(f"wrote {COMMANDS_MD} ({len(doc)} bytes)")
