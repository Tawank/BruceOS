"""Regenerates docs/COMMANDS.md by booting the QEMU firmware straight into
`man --gen-md --line-marker`.

This isn't a correctness test -- it doesn't assert anything about firmware
behavior, it captures --gen-md's output to a file. It's marked `docs` so
conftest.py's `_reconfigure_boot_command` autouse fixture rebuilds
build-qemu/ with CONFIG_BRUCE_QEMU_TEST_BOOT_COMMAND="man --gen-md
--line-marker" before this test's `dut` fixture boots it -- main.c launches
that command itself right after "QEMU READY" (there's no way to type it
into an already-booted shell; see conftest.py's docstring on
_ensure_boot_command for why).

--line-marker: QEMU's virtual UART / this harness's pty capture path
occasionally injects a stray extra newline into the raw captured byte
stream (confirmed, by instrumenting every write() syscall in the firmware,
to never originate from BruceOS's own code -- see man_app.c's
MAN_APP_GEN_MD_BUF_CAPACITY doc comment for the full investigation). With
--line-marker, man never writes a real '\\n' for --gen-md's output at all --
every line break is LINE_MARKER below instead (see man_app.c's
MAN_APP_GEN_MD_LINE_MARKER, which this constant must match) -- so a real
'\\n' can only be that artifact: it's dropped, then the marker is turned
back into '\\n', once capture is complete and there's no more ambiguity to
worry about. This is not the same thing as guessing which blank lines
"look" spurious in the finished text; every raw '\\n' byte in the capture is
unconditionally an artifact under this scheme, full stop.

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
# ASCII Record Separator -- must match man_app.c's MAN_APP_GEN_MD_LINE_MARKER.
LINE_MARKER = "\x1e"
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
    doc = text[start:]

    # See this file's module docstring: --line-marker means every line break
    # --gen-md actually wrote survives as LINE_MARKER, never a real '\n' --
    # so any '\n' still in here is unconditionally a capture-path artifact,
    # not a judgment call about which blank lines look wrong.
    doc = doc.replace("\n", "").replace(LINE_MARKER, "\n")
    doc = doc.rstrip("\n") + "\n"

    COMMANDS_MD.write_text(doc, encoding="utf-8")
    print(f"wrote {COMMANDS_MD} ({len(doc)} bytes)")
