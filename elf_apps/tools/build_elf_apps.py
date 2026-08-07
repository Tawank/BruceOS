#!/usr/bin/env python3
"""Build the SDK ELF app templates and inject the .bruce.manifest section.

Usage:
    python3 elf_apps/tools/build_elf_apps.py [--target esp32s3]

The script builds the ELF examples listed in APPS, then
injects their manifest.json files into the resulting ELF images as a
non-allocatable .bruce.manifest section.  Final outputs are written to:

    elf_apps/examples/elf_loader.elf
    elf_apps/examples/game.elf
    elf_apps/examples/nes.elf

The firmware's built-in "elf" command can then load them:

    elf ./elf_loader.elf ./game.elf
"""

import argparse
import os
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SDK_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
APPS_DIR = os.path.join(SDK_DIR, "examples")
APPS = ["nes"]  # "elf_loader", "game",


def run(cmd, cwd, env=None):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=cwd, env=env)


def build_app(app, target, idf_path):
    app_dir = os.path.join(APPS_DIR, app)
    build_dir = os.path.join(app_dir, "build")
    idf_py = os.path.join(idf_path, "tools", "idf.py")
    if not os.path.exists(idf_py):
        raise RuntimeError(f"idf.py not found at {idf_py}")

    env = os.environ.copy()
    env["IDF_PATH"] = idf_path
    env["IDF_TARGET"] = target

    # Set target and reconfigure. This downloads managed_components.
    run([sys.executable, idf_py, "-B", build_dir, "set-target", target], app_dir, env)

    # Apply the same ESP-IDF v6 patch the main firmware uses, before build.
    patch_path = os.path.join(SDK_DIR, "..", "patches", "elf_loader-v1.3.1-idf-v6.patch")
    app_elf_loader_dir = os.path.join(app_dir, "managed_components", "espressif__elf_loader")
    if os.path.isdir(app_elf_loader_dir) and os.path.exists(patch_path):
        try:
            subprocess.check_call(
                ["patch", "-d", app_elf_loader_dir, "-p1", "-N", "--dry-run"],
                stdin=open(patch_path, "rb"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.check_call(
                ["patch", "-d", app_elf_loader_dir, "-p1", "-N"],
                stdin=open(patch_path, "rb"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f"Applied ELF loader patch for {app}")
        except subprocess.CalledProcessError:
            pass

    # Build the ELF app target.
    run([sys.executable, idf_py, "-B", build_dir, "elf"], app_dir, env)

    # Default output from project_elf(app) is build/app.app.elf.
    src_elf = os.path.join(build_dir, f"{app}.app.elf")
    if not os.path.exists(src_elf):
        raise RuntimeError(f"Build output not found: {src_elf}")

    # Inject the .bruce.manifest section without SHF_ALLOC.
    manifest_path = os.path.join(app_dir, "manifest.json")
    if not os.path.exists(manifest_path):
        raise RuntimeError(f"Manifest file not found: {manifest_path}")

    # We must work on a copy: objcopy --add-section on the same file can be
    # unreliable across toolchains.
    tmp_elf = os.path.join(build_dir, f"{app}.tmp.elf")
    shutil.copy2(src_elf, tmp_elf)

    if target.startswith("esp32c") or target == "esp32p4":
        prefix = "riscv32-esp-elf"
    else:
        prefix = f"xtensa-{target}-elf"
    objcopy = f"{prefix}-objcopy"

    run([
        objcopy,
        "--add-section", f".bruce.manifest={manifest_path}",
        "--set-section-flags", ".bruce.manifest=contents,readonly",
        tmp_elf,
        src_elf,
    ], build_dir, env)

    os.remove(tmp_elf)

    out_elf = os.path.join(APPS_DIR, f"{app}.elf")
    shutil.copy2(src_elf, out_elf)
    print(f"Built {out_elf}")


def main():
    parser = argparse.ArgumentParser(description="Build Bruce ELF app templates")
    parser.add_argument("--target", default="esp32s3", help="ESP-IDF target (default: esp32s3)")
    parser.add_argument("--idf-path", default=os.environ.get("IDF_PATH", ""), help="Path to ESP-IDF")
    args = parser.parse_args()

    if not args.idf_path or not os.path.isdir(args.idf_path):
        print("IDF_PATH not set or invalid", file=sys.stderr)
        sys.exit(1)

    for app in APPS:
        build_app(app, args.target, args.idf_path)

    print("Done. Outputs:")
    for app in APPS:
        print(f"  {os.path.join(APPS_DIR, app + '.elf')}")


if __name__ == "__main__":
    main()
