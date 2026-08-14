import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[1] / "native_apps" / "tools"))

import wasm_compiler


def test_falls_back_when_path_clang_has_no_wasm(monkeypatch):
    executables = {"clang": "/esp/clang", "/usr/bin/clang": "/usr/bin/clang"}
    monkeypatch.setattr(wasm_compiler.shutil, "which", lambda value: executables.get(value))

    def run(command, **kwargs):
        targets = "  xtensa - Xtensa\n" if command[0] == "/esp/clang" else "  wasm32 - WebAssembly 32-bit\n"
        return subprocess.CompletedProcess(command, 0, stdout=targets)

    monkeypatch.setattr(wasm_compiler.subprocess, "run", run)
    assert wasm_compiler.resolve_wasm_compiler("clang", ("/usr/bin/clang",)) == "/usr/bin/clang"


def test_rejects_explicit_compiler_without_wasm(monkeypatch):
    monkeypatch.setattr(wasm_compiler.shutil, "which", lambda value: "/custom/clang")
    monkeypatch.setattr(
        wasm_compiler.subprocess,
        "run",
        lambda command, **kwargs: subprocess.CompletedProcess(command, 0, stdout="  xtensa - Xtensa\n"),
    )
    with pytest.raises(RuntimeError, match="does not support wasm32"):
        wasm_compiler.resolve_wasm_compiler("/custom/clang")
