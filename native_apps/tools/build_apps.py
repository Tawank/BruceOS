#!/usr/bin/env python3
"""Build Bruce external applications for native ELF or WASM targets."""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
APPS_DIR = SCRIPT_DIR.parent / "examples"
WASM_GUEST_SOURCE = (
    SCRIPT_DIR.parent.parent / "src" / "modules" / "loaders" / "wasm" / "wasm_bruce_guest_adapter.c"
)


def available_apps():
    return sorted(p.name for p in APPS_DIR.iterdir() if p.is_dir())


def run(command, cwd, env=None):
    print("+", " ".join(str(part) for part in command))
    subprocess.check_call(command, cwd=cwd, env=env)


def leb(value):
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        result.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(result)


def add_wasm_manifest(wasm_path, manifest_path):
    wasm = wasm_path.read_bytes()
    manifest = manifest_path.read_bytes()
    section_name = b"bruce.manifest"
    payload = leb(len(section_name)) + section_name + manifest
    wasm_path.write_bytes(wasm + b"\x00" + leb(len(payload)) + payload)


def read_manifest(app_dir):
    manifest_path = app_dir / "manifest.json"
    if not manifest_path.exists():
        raise RuntimeError(f"Manifest file not found: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Invalid manifest: {manifest_path}") from error
    entry = manifest.get("entryPoint", "app_main")
    if not isinstance(entry, str) or not entry.isidentifier():
        raise RuntimeError(f"Invalid entryPoint in {manifest_path}")
    return manifest_path, entry


def build_native(app, target, idf_path):
    app_dir = APPS_DIR / app
    build_dir = app_dir / "build"
    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.exists():
        raise RuntimeError(f"idf.py not found at {idf_py}")

    env = os.environ.copy()
    env["IDF_PATH"] = idf_path
    env["IDF_TARGET"] = target
    run([sys.executable, idf_py, "-B", build_dir, "set-target", target], app_dir, env)
    run([sys.executable, idf_py, "-B", build_dir, "elf"], app_dir, env)

    source_elf = build_dir / f"{app}.app.elf"
    manifest, _ = read_manifest(app_dir)
    if not source_elf.exists():
        raise RuntimeError(f"Missing ELF output or manifest for {app}")

    prefix = "riscv32-esp-elf" if target.startswith("esp32c") or target == "esp32p4" else f"xtensa-{target}-elf"
    temporary = build_dir / f"{app}.tmp.elf"
    shutil.copy2(source_elf, temporary)
    run([
        f"{prefix}-objcopy", "--add-section", f".bruce.manifest={manifest}",
        "--set-section-flags", ".bruce.manifest=contents,readonly",
        temporary, source_elf,
    ], build_dir, env)
    temporary.unlink()
    output = APPS_DIR / f"{app}.elf"
    shutil.copy2(source_elf, output)
    print(f"Built {output}")


def build_wasm(app, compiler):
    app_dir = APPS_DIR / app
    source_dir = app_dir / "main"
    manifest, entry = read_manifest(app_dir)
    sources = sorted(
        path for path in source_dir.rglob("*")
        if path.suffix in (".c", ".cc", ".cpp", ".cxx")
    )
    if not sources:
        raise RuntimeError(
            f"{app} has no C/C++ sources under {source_dir}"
        )
    cxx_sources = [path for path in sources if path.suffix != ".c"]
    if cxx_sources:
        raise RuntimeError(
            "C++ WASM inputs are not supported yet: "
            + ", ".join(str(path.relative_to(app_dir)) for path in cxx_sources)
        )
    output = APPS_DIR / f"{app}.wasm"
    run([
        compiler, "--target=wasm32", "-O2", "-nostdlib", "-ffreestanding",
        f"-D{entry}=main",
        "-Wl,--no-entry", "-Wl,--allow-undefined", "-Wl,--export=main",
        "-Wl,--export-memory", "-Wl,--initial-memory=65536",
        "-Wl,--max-memory=262144", "-I", SCRIPT_DIR.parent / "include",
        "-I", SCRIPT_DIR.parent.parent / "src",
        "-o", output, *sources, WASM_GUEST_SOURCE,
    ], app_dir)
    add_wasm_manifest(output, manifest)
    print(f"Built {output}")


def main():
    parser = argparse.ArgumentParser(description="Build Bruce external apps")
    parser.add_argument("--target", default="elf", help="elf, wasm, or an ESP-IDF target")
    parser.add_argument("--idf-target", default="esp32s3", help="ESP-IDF chip target for --target elf")
    parser.add_argument("--idf-path", default=os.environ.get("IDF_PATH", ""))
    parser.add_argument("--compiler", default=os.environ.get("WASM_CLANG", "clang"))
    parser.add_argument("--app", action="append", choices=available_apps())
    args = parser.parse_args()
    apps = list(dict.fromkeys(args.app or available_apps()))

    if args.target == "wasm":
        if shutil.which(args.compiler) is None:
            raise SystemExit(f"WASM compiler not found: {args.compiler}")
        for app in apps:
            build_wasm(app, args.compiler)
    else:
        if not args.idf_path or not Path(args.idf_path).is_dir():
            raise SystemExit("IDF_PATH not set or invalid")
        native_target = args.idf_target if args.target == "elf" else args.target
        for app in apps:
            build_native(app, native_target, args.idf_path)


if __name__ == "__main__":
    main()
