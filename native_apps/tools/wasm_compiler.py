"""Locate a Clang executable with the WebAssembly backend enabled."""

import shutil
import subprocess
from pathlib import Path


def _supports_wasm32(compiler):
    try:
        result = subprocess.run(
            [compiler, "--print-targets"],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return False
    return result.returncode == 0 and any(
        line.strip().split(maxsplit=1)[0] == "wasm32"
        for line in result.stdout.splitlines()
        if line.strip()
    )


def resolve_wasm_compiler(requested, fallback_candidates=None):
    if fallback_candidates is None:
        fallback_candidates = (Path("/usr/bin/clang"), *(f"clang-{version}" for version in range(22, 13, -1)))

    candidates = (requested, *fallback_candidates) if requested == "clang" else (requested,)
    checked = []
    for candidate in candidates:
        candidate = str(candidate)
        executable = shutil.which(candidate)
        if executable is None or executable in checked:
            continue
        checked.append(executable)
        if _supports_wasm32(executable):
            return executable

    if not checked:
        raise RuntimeError(f"WASM compiler not found: {requested}")
    raise RuntimeError(
        f"WASM compiler does not support wasm32: {requested}; "
        "install a full LLVM build or pass --compiler /path/to/clang"
    )
